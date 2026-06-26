/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2023, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2023, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for pthread_getattr_np */
#endif
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <alaska/StackMapParser.h>
#include <alaska/config.h>


#include <dlfcn.h>

#include <alaska.h>
#include <alaska/utils.h>
#include <alaska/alaska.hpp>
#include <alaska/rt/barrier.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadRegistry.hpp>
#include <alaska/gc_bitmaps.hpp>

#include <ck/lock.h>
#include <ck/map.h>
#include <ck/set.h>
#include <ck/vec.h>
#include <ck/func.h>

#include <ucontext.h>
#include <execinfo.h>
#include <pthread.h>
#include <alaska/list_head.h>
#include <stdbool.h>
#include <sys/signal.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>


// --- liballocs/systrap coordination -----------------------------------------
// When stackscan runs under liballocs, every libc syscall is trapped and emulated
// inside systrap's SIGILL handler; for blocking syscalls (sleep/futex/...) the
// thread is parked *inside* that handler. If our barrier SIGUSR2 interrupts such
// a thread and we unwind across the nested handler + kernel signal frames, we
// corrupt its resume state (observed as flaky SIGSEGV rip=0 / self-SIGPIPE /
// abort). systrap publishes, per-thread, the saved application SP/IP while it is
// emulating; we weak-reference the accessors so this is a no-op when liballocs is
// absent (symbols resolve to NULL). A thread with a non-NULL saved SP is parked
// in a syscall -> at a GC-safe point -> we scan its roots conservatively from the
// saved context instead of unwinding.
extern "C" void *__systrap_current_saved_sp(void) __attribute__((weak));
extern "C" void *__systrap_current_saved_ip(void) __attribute__((weak));

// Diagnostic A/B gate (read once at init). With STACKSCAN_DIAG_SKIP_UNWIND set,
// an in-systrap thread skips even the conservative root scan (still joins the
// barrier) -- to confirm the root collection is not itself a crash source.
static int diag_skip_unwind = 0;
__attribute__((constructor)) static void alaska_barrier_diag_init(void) {
  diag_skip_unwind = (getenv("STACKSCAN_DIAG_SKIP_UNWIND") != nullptr);
}

// Returns the saved application stack pointer iff this thread is currently parked
// inside a systrap syscall emulation, else NULL (also NULL when liballocs absent,
// since the accessor is a weak undefined symbol then).
static inline void *thread_in_systrap_sp(void) {
  if (&__systrap_current_saved_sp == nullptr) return nullptr; // liballocs absent
  return __systrap_current_saved_sp();
}


enum class StackState {
  ManagedTracked,    // The thread is in managed code, at a poll. (safe to join barrier)
  ManagedUntracked,  // The thread is in managed code, but not at a poll
  Unmanaged,         // The thread is in unmanaged code. (save to join barrier)
};


struct PinSetInfo {
  uint32_t count = 0;   // How many entries?
  uint32_t regNum = 0;  // Which libunwind register?
  int32_t offset = 0;   // offset from that register?
};

struct StackMapping {
  bool direct;
  uint32_t regNum;
  int32_t offset;
  uint64_t id;
};

struct ManagedBlobRegion {
  uintptr_t start, end;
};


static ck::vec<ManagedBlobRegion> managed_blob_text_regions;
static ck::map<uintptr_t, PinSetInfo> pin_map;
// the return addresses from calls to potentially blocking functions
static ck::set<uintptr_t> block_rets;


#ifdef __amd64__
using inst_t = uint16_t;
#endif
#ifdef __aarch64__
using inst_t = uint32_t;
#endif
#ifdef __riscv
using inst_t = uint16_t;
#endif

struct PatchPoint {
  inst_t* pc;
  inst_t inst_nop;
  inst_t inst_sig;
};
static ck::vec<PatchPoint> patchPoints;



static void patchSignal() {
  for (auto& p : patchPoints) {
    __atomic_store_n(p.pc, p.inst_sig, __ATOMIC_RELEASE);
  }
}


static void patchNop(void) {
  for (auto& p : patchPoints) {
    __atomic_store_n(p.pc, p.inst_nop, __ATOMIC_RELEASE);
  }
}

static void setup_signal_handlers(void);
static void clear_pending_signals(void);



enum class JoinReason { Signal, Safepoint };


#define ALASKA_THREAD_TRACK_STATE_T AlaskaThreadState
#define ALASKA_THREAD_TRACK_INIT setup_signal_handlers();
#include <alaska/thread_tracking.in.hpp>




static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;

// --- Robust stop-the-world rendezvous --------------------------------------
// Replaces a fragile pthread_barrier_t that was destroyed/re-init'd inside
// begin() while threads could still be blocked on it (a guaranteed deadlock at
// thread-count changes), and that required *exactly* num_threads participants --
// so any single thread that could not answer the barrier promptly (e.g. one with
// SIGUSR2 masked inside a runtime lock, or stuck at JOIN_REASON_ABORT)
// permanently wedged the rendezvous.
//
// The orchestrator (barrier_thread_func) signals each mutator with SIGUSR2; each
// runs the signal handler, pins its stack roots, then *parks* here until the
// orchestrator finishes its callback and releases the world. The orchestrator
// waits only for the threads that actually arrived (rv_parked), counted from
// their join_status -- never for a fixed num_threads -- and these objects are
// never destroyed, so a slow or never-arriving thread cannot deadlock it.
static pthread_mutex_t rv_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  rv_cv  = PTHREAD_COND_INITIALIZER;
static int             rv_parked   = 0;     // # participants currently parked (arrived, not released)
static bool            rv_released = true;   // true between barriers: a late/stale arrival must NOT park

// Orchestrator: open the rendezvous for a new stop-the-world (call before any
// thread can be signalled). After this, arriving participants park.
static void rv_begin(void) {
  pthread_mutex_lock(&rv_mtx);
  rv_released = false;
  pthread_mutex_unlock(&rv_mtx);
}

// Participant: park until the orchestrator releases the world, then depart. Run
// from the barrier signal handler; safe because the handler is non-reentrant
// (SIGUSR2/SIGILL are masked in it) and the orchestrator -- which also takes
// rv_mtx -- is never itself signalled.
//
// If the barrier is already releasing (rv_released) we must NOT park: this is a
// late arrival (a safepoint poll taken after the orchestrator finished counting)
// or a stale re-delivered signal. Parking now would wait for a release that has
// already happened -- the lost-wakeup deadlock. Returning is safe: the world is
// resuming anyway, and our roots were pinned/unpinned around this call.
static void rv_park(void) {
  pthread_mutex_lock(&rv_mtx);
  if (rv_released) { pthread_mutex_unlock(&rv_mtx); return; }
  rv_parked++;
  pthread_cond_broadcast(&rv_cv);  // notify orchestrator of arrival
  while (!rv_released) pthread_cond_wait(&rv_cv, &rv_mtx);
  rv_parked--;
  pthread_cond_broadcast(&rv_cv);  // notify orchestrator of departure
  pthread_mutex_unlock(&rv_mtx);
}

// Orchestrator: block until at least `expected` participants have parked. Called
// after the signal loop has driven every participant into the handler, so the
// count is stable; guarantees all participants have pinned their roots before
// the callback (compaction/reclaim) runs.
static void rv_wait_all_parked(int expected) {
  pthread_mutex_lock(&rv_mtx);
  while (rv_parked < expected) pthread_cond_wait(&rv_cv, &rv_mtx);
  pthread_mutex_unlock(&rv_mtx);
}

// Orchestrator: release every parked participant and wait for them all to leave,
// so no participant is still touching pinned state when the world resumes.
static void rv_release_all(void) {
  pthread_mutex_lock(&rv_mtx);
  rv_released = true;
  pthread_cond_broadcast(&rv_cv);
  while (rv_parked > 0) pthread_cond_wait(&rv_cv, &rv_mtx);
  pthread_mutex_unlock(&rv_mtx);
}




void alaska_remove_from_local_lock_list(void* ptr) { return; }
static void alaska_dump_thread_states_r(void) {
  struct info* pos;
  alaska::thread_tracking::threads().for_each_locked([&](auto thread, AlaskaThreadState* state) {
    if (state->escaped == 0) {
      printf("\e[0m. ");  // a thread will join a barrier (not out to lunch)
    } else {
      printf("\e[41m. ");  // a thread will need to be interrupted (out to lunch)
    }
  });
  printf("\e[0m\n");
}

void alaska_dump_thread_states(void) {
  auto lk = alaska::thread_tracking::threads().take_lock();
  alaska_dump_thread_states_r();
}


static StackState get_stack_state(uintptr_t return_address) {
  if (pin_map.contains(return_address)) {
    return StackState::ManagedTracked;
  }

  for (auto [start, end] : managed_blob_text_regions) {
    if (return_address >= start && return_address < end) {
      return StackState::ManagedUntracked;
    }
  }

  return StackState::Unmanaged;
}



static void record_handle(void* possible_handle, bool marked) {
  alaska::Mapping* m = alaska::Mapping::from_handle_safe(possible_handle);

  // It wasn't a handle, don't consider it. is_live_handle (not just valid_handle + !is_free):
  // a stack word can decode to a slot currently on the allocator's free list, whose word is a
  // raw next-link with the invl bit clear -- is_free() would pass it and set_pinned() below
  // would CAS bit 61 into that link, corrupting the free list. See HandleTable::is_live_handle.
  if (not alaska::Runtime::get().handle_table.is_live_handle(m)) return;
  m->set_pinned(marked);
}



static bool might_be_handle(void* possible_handle) {
  alaska::Mapping* m = alaska::Mapping::from_handle_safe(possible_handle);
  return m != nullptr;
}


static ck::mutex dump_lock;

static bool in_might_block_function(uintptr_t start_addr) {
  void* buffer[512];

  int depth = backtrace(buffer, 512);
  dump_lock.lock();
  bool found_start = false;
  for (int i = 0; i < depth; i++) {
    auto addr = (uintptr_t)buffer[i];
    if (addr == start_addr) {
      found_start = true;
    }
    if (!found_start) continue;

    const char* msg = "\e[33m(unmanaged)\e[0m";

    for (auto [start, end] : managed_blob_text_regions) {
      if (addr >= start && addr < end) {
        // red
        msg = "\e[31m(managed)\e[0m";
        break;
      }
    }
    if (pin_map.contains(addr)) {
      // green
      msg = "\e[32m(managed)\e[0m";
    }

    printf("%016lx %s\n", (uintptr_t)buffer[i], msg);
  }
  printf("\n");

  dump_lock.unlock();


  for (int i = 0; i < depth; i++) {
    if (block_rets.contains((uintptr_t)buffer[i])) {
      return true;
    }
  }

  return false;
}

void alaska::barrier::get_pinned_handles(ck::set<void*>& out) {
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_word_t pc, sp, reg;

  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  while (1) {
    int res = unw_step(&cursor);
    if (res == 0) {
      break;
    }
    if (res < 0) {
      printf("unknown libunwind error! %d\n", res);
      abort();
    }
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    unw_get_reg(&cursor, UNW_REG_SP, &sp);
    // printf("pc:%016lx sp:%016lx ", pc, sp);
    auto it = pin_map.find(pc);
    if (it != pin_map.end()) {
      auto& psi = it->value;
      if (psi.count == 0) continue;  // should not happen!
      unw_get_reg(&cursor, psi.regNum, &reg);


      void** localSet = (void**)(reg + psi.offset);

      for (uint32_t i = 0; i < psi.count; i++) {
        if (might_be_handle(localSet[i])) {
          out.add(localSet[i]);
        }
      }
    }
  }
}



// Conservatively pin (marked=true) or unpin (marked=false) every handle found on
// the stack range [sp_lo, sp_hi) of a thread parked inside a systrap syscall
// emulation. This must be MALLOC-FREE: such a thread may be parked in an
// mmap/brk syscall *issued by malloc*, holding the allocator lock, so building a
// ck::set here (which allocates) would deadlock or corrupt. record_handle only
// does a bounds-checked table lookup + a bit set/clear (no locks, no allocation),
// and ignores non-handles, so we call it directly. Over-approximation is GC-safe
// (never under-pins). We re-scan to unpin rather than remembering a set.
static void mark_conservative_range(void* sp_lo, void* sp_hi, bool marked) {
  if (!sp_lo || !sp_hi || sp_lo >= sp_hi) return;
  for (void** p = (void**) sp_lo; p < (void**) sp_hi; ++p) {
    record_handle(*p, marked);
  }
}


// --- Stackscan "present" scan during the barrier ------------------------------
// The stackscan collector reclaims zero-refcount handles that are not on any
// thread's stack. It reuses THIS barrier: while the world is stopped, each
// participating thread conservatively scans its own [sp, stack_top) + saved
// registers and marks every handle-looking word into the present bitmap (a safe
// superset of its true stack roots -- never frees a live handle, and independent
// of the precise `pinned` bit the cycle collector uses). The reclaim pass then
// runs in the barrier callback (alaska::reclaim_dead_handles). The scan runs
// inside the existing barrier signal handler -- no extra signal, no extra
// rendezvous (rv_park is the rendezvous). g_present_scan is set by
// barrier_thread_func only on reclaim cycles so non-reclaim barriers pay nothing.
// The present-bitmap stackscan exists only to support refcount reclamation
// (alaska::gc::present_* live in gc_bitmaps.cpp, which is compiled only when
// ALASKA_ENABLE_REFCOUNT is on). Anchorage-only / noservice builds have no
// reclaim pass, so the whole present-scan subsystem is compiled out -- otherwise
// libalaska would carry an undefined reference to alaska::gc::present_mark.
#if ALASKA_ENABLE_REFCOUNT
static volatile int g_present_scan = 0;

// Mark one candidate word present iff it is a live (allocated, non-free) handle.
// Mirrors record_handle's checks but targets the present bitmap. Async-signal-safe
// (is_live_handle does only lock-free loads + pointer arithmetic, no allocation).
static inline void scan_mark_present(void* possible_handle) {
  alaska::Mapping* m = alaska::Mapping::from_handle_safe(possible_handle);
  if (not alaska::Runtime::get().handle_table.is_live_handle(m)) return;
  alaska::gc::present_mark(m);
}

static void scan_conservative_present(void* lo, void* hi) {
  if (!lo || !hi || lo >= hi) return;
  for (void** p = (void**)lo; p < (void**)hi; ++p) {
    scan_mark_present(*p);
  }
}

static void scan_registers_present(ucontext_t* uc) {
#if defined(__amd64__)
  for (int i = 0; i < NGREG; i++) {
    scan_mark_present((void*)uc->uc_mcontext.gregs[i]);
  }
#endif
}
#endif  // ALASKA_ENABLE_REFCOUNT

// Conservatively pin/unpin every handle-looking value held in the interrupted
// thread's registers (companion to mark_conservative_range for the stack). Used
// by the barrier handler's normal path instead of libunwind-based precise
// pinning, which is not signal-reentrant.
static void mark_registers_conservative(ucontext_t* uc, bool marked) {
#if defined(__amd64__)
  for (int i = 0; i < NGREG; i++) {
    record_handle((void*)uc->uc_mcontext.gregs[i], marked);
  }
#endif
}

// Called from the barrier signal handler on each participating thread when a
// reclaim cycle is active: conservatively mark this thread's stack+register
// handles present. `sp_lo` is the thread's stack pointer at the interrupt
// (systrap saved SP, or ucontext RSP); registers are only scanned when not parked
// in systrap (then the live registers are the app's, not the handler's).
#if ALASKA_ENABLE_REFCOUNT
static inline void present_scan_self(void* sp_lo, ucontext_t* uc, bool from_systrap) {
  if (!g_present_scan) return;
  void* sp_hi = alaska::thread_tracking::my_state.stack_top;
  scan_conservative_present(sp_lo, sp_hi);
  if (!from_systrap) scan_registers_present(uc);
}

extern "C" void alaska_gc_present_scan_set(int on) {
  __atomic_store_n(&g_present_scan, on, __ATOMIC_RELEASE);
}

// The main executable's initialized data + BSS hold handle-typed C globals, which are GC roots
// exactly like the stack and registers -- but nothing else scans them. A handle reachable ONLY
// through a global (or a refcount-0 handle parked there, e.g. a long-lived root in a static) is
// otherwise invisible to the present-set and reclaim_dead_handles frees it. Conservatively mark
// every handle-looking word in [__data_start, _end) present, ONCE per barrier (globals are
// shared, so the orchestrator does this rather than every participant -- unlike the per-thread
// stack/register scan above). Linker-provided bounds for the main image; this does NOT cover
// shared-library data segments (libalaska/libc globals are runtime-internal, not app roots).
// Conservative, so any false positive only over-retains. Pinning is NOT needed here: handles are
// stable across compaction, so a global's handle value stays valid even if its object moves.
// Weak so an image without these standard linker symbols still loads (they then resolve to 0 and
// the scan is skipped). If they resolve to libalaska's own data rather than the executable's it is
// still safe -- a conservative over-scan only over-retains; for a precise multi-image scan switch
// to dl_iterate_phdr over each object's writable PT_LOAD.
extern "C" char __data_start[] __attribute__((weak));
extern "C" char _end[] __attribute__((weak));
extern "C" void alaska_gc_scan_globals_present(void) {
  if (!g_present_scan) return;
  void *lo = (void *)__data_start, *hi = (void *)_end;
  if (lo == nullptr || hi == nullptr || lo >= hi) return;  // symbols absent/unresolved
  scan_conservative_present(lo, hi);
}
#else
// No reclaim pass without reference counting: the barrier handler still calls
// present_scan_self on every participating thread, so keep it as a no-op.
static inline void present_scan_self(void*, ucontext_t*, bool) {}
#endif  // ALASKA_ENABLE_REFCOUNT


// Pin / unpin this participant's roots in the global handle table. The actual
// stop-the-world wait is the rv_park()/rv_*() rendezvous above, so these are now
// pure pin / unpin helpers (no barrier wait).
static void participant_pin(const ck::set<void*>& ps) {
  for (auto* p : ps) record_handle(p, true);
}

static void participant_unpin(const ck::set<void*>& ps) {
  for (auto* p : ps) record_handle(p, false);
}


void dump_thread_states(void) {
  struct info* pos;
  alaska::thread_tracking::threads().for_each_locked([&](auto thread, AlaskaThreadState* state) {
    switch (state->join_status) {
      case ALASKA_JOIN_REASON_NOT_JOINED:
        printf("\e[41m! ");  // a thread will need to be interrupted (out to lunch)
        break;

      case ALASKA_JOIN_REASON_SIGNAL:
        printf("\e[41m. ");  // a thread will need to be interrupted (out to lunch)
        break;

      case ALASKA_JOIN_REASON_SAFEPOINT:
        printf("\e[42m. ");  // a thread will join a barrier (not out to lunch)
        break;

      case ALASKA_JOIN_REASON_ORCHESTRATOR:
        printf("\e[44m. ");  // a thread will join a barrier (not out to lunch)
        break;
    }
  });
  printf("\e[0m");
}



bool alaska::barrier::begin(void) {
  // Pseudocode:
  //
  // function begin():
  //   patch();
  //   while not everyone_joined():
  //      usleep(n);
  //      for thread in threads:
  //        if not thread->joined:
  //          signal(thread);
  //   synch();


  // Take locks so nobody else tries to signal a barrier.
  pthread_mutex_lock(&barrier_lock);
  alaska::thread_tracking::threads().lock_thread_creation();



  auto num_threads = alaska::thread_tracking::threads().num_threads();
#if 0
  alaska::printf("Barrier begin from %lx:\n", pthread_self());
  alaska::printf("  num threads: %lu\n", num_threads);

  printf("  threads:\n");
  alaska::thread_tracking::threads().for_each_locked([](auto thread, AlaskaThreadState* state) {
    alaska::printf("  - %lx %p %d\n", thread, state, state->join_status);
  });
#endif


  // First, mark everyone as *not* in the barrier.
  alaska::thread_tracking::threads().for_each_locked([](auto thread, AlaskaThreadState* state) {
    state->join_status = ALASKA_JOIN_REASON_NOT_JOINED;
  });

  // Mark the orch thread (us) as joined
  alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_ORCHESTRATOR;

  // Open the rendezvous before anyone can be signalled, so arriving participants
  // park rather than skip.
  rv_begin();

  // now, patch the threads!
  patchSignal();
  int retries = 0;
  int signals_sent = 0;

  bool success = true;

  // Make sure the threads that are in unmanaged (library) code get signalled.
  while (true) {
    if (retries >= 1000) {
      success = false;
      break;
    }
    retries++;
    bool sent_signal = false;
    bool aborted = false;

    alaska::thread_tracking::threads().for_each_locked([&](auto thread, auto* state) {
      if (state->join_status == ALASKA_JOIN_REASON_NOT_JOINED) {
        pthread_kill(thread, SIGUSR2);
        sent_signal = true;
        signals_sent++;
      }

      if (state->join_status == ALASKA_JOIN_REASON_ABORT) {
        success = false;
        printf("Abort barrier. Could not join all threads for some reason!\n");
      }
    });
    if (!sent_signal) break;
    usleep(1000);  // lazy optimization
  }


  // Count the participants that actually joined (set a join reason other than
  // ORCHESTRATOR). The rendezvous waits for exactly these -- not num_threads --
  // so a thread that never answered (still NOT_JOINED at the retry cap) cannot
  // wedge the barrier; it is simply not part of this stop-the-world.
  int joined = 0;
  alaska::thread_tracking::threads().for_each_locked([&](auto thread, auto* state) {
    if (state->join_status != ALASKA_JOIN_REASON_NOT_JOINED &&
        state->join_status != ALASKA_JOIN_REASON_ORCHESTRATOR) {
      joined++;
    }
  });

  // Pin the orchestrator's own roots, then wait until every joined participant
  // has parked (and therefore pinned its roots) before returning to the caller,
  // which runs the stop-the-world callback.
  ck::set<void*> locked;
  alaska::barrier::get_pinned_handles(locked);  // TODO: slow!
  participant_pin(locked);
  rv_wait_all_parked(joined);

  (void)retries;
  (void)signals_sent;
  (void)num_threads;


  return success;
}




void alaska::barrier::end(void) {
  patchNop();

  // Unpin the orchestrator's own roots, then release every parked participant
  // and wait for them all to leave before letting the world run again.
  ck::set<void*> locked;
  alaska::barrier::get_pinned_handles(locked);  // TODO: slow!
  participant_unpin(locked);
  rv_release_all();

  // Unlock all the locks we took.
  alaska::thread_tracking::threads().unlock_thread_creation();
  pthread_mutex_unlock(&barrier_lock);
}




void alaska_barrier(void) {
  // alaska::service::barrier();
}


thread_local bool invalid_state_abort = false;

// Re-entrancy guard for the barrier handler. The handler blocks in futex syscalls
// (rv_park's mutex/cond). Under liballocs/systrap those syscalls are emulated with
// the application signal mask -- which does NOT include our sa_mask -- so a second
// SIGUSR2 can be delivered *while the handler is mid-rv_park*, re-entering the
// handler and self-deadlocking on the (non-recursive) rv_mtx it already holds.
// A nested invocation is always a spurious re-delivery of the same barrier (a
// thread participates in at most one barrier at a time and cannot start a new one
// while parked), so it is safe -- and necessary -- to make it a no-op.
static thread_local int in_barrier_handler = 0;

static void alaska_barrier_signal_handler_impl(int sig, siginfo_t* info, void* ptr);

static void alaska_barrier_signal_handler(int sig, siginfo_t* info, void* ptr) {
  if (in_barrier_handler) return;
  in_barrier_handler = 1;
  alaska_barrier_signal_handler_impl(sig, info, ptr);
  in_barrier_handler = 0;
}

static void alaska_barrier_signal_handler_impl(int sig, siginfo_t* info, void* ptr) {
  ucontext_t* ucontext = (ucontext_t*)ptr;

  // --- In-systrap fast path -------------------------------------------------
  // If this thread is parked inside a liballocs/systrap syscall emulation it is
  // at a GC-safe point (blocked in a syscall, not mutating the managed heap).
  // We must NOT unwind the live stack here: that would cross the nested systrap
  // and kernel signal frames and corrupt the thread's resume (the source of the
  // flaky SIGSEGV rip=0 / self-SIGPIPE / abort). Instead scan roots
  // conservatively from systrap's saved application context, then join.
  {
    void *saved_sp = thread_in_systrap_sp();
    if (saved_sp) {
      // Parked in a syscall (a libc syscall site, i.e. unmanaged) -> join as a
      // signalled participant. Everything here is malloc-free and uses no
      // libunwind (see mark_conservative_range): we must not allocate or unwind on
      // a thread parked mid-syscall.
      alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_SIGNAL;
      void *sp_hi = alaska::thread_tracking::my_state.stack_top;
      if (!diag_skip_unwind) mark_conservative_range(saved_sp, sp_hi, /*pin*/ true);
      // Stackscan: mark this thread's stack handles present (before parking, so all
      // marks are committed before the orchestrator runs the reclaim callback).
      present_scan_self(saved_sp, nullptr, /*from_systrap=*/true);
      rv_park();  // wait until the orchestrator releases the world
      if (!diag_skip_unwind) mark_conservative_range(saved_sp, sp_hi, /*unpin*/ false);
      clear_pending_signals();
      return;
    }
  }

  uintptr_t return_address = 0;

#if defined(__amd64__)
  return_address = ucontext->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  return_address = ucontext->uc_mcontext.pc;
#elif defined(__riscv)
  return_address = ucontext->uc_mcontext.__gregs[REG_PC];
#endif

  auto state = get_stack_state(return_address);

  switch (state) {
    case StackState::ManagedUntracked:
      // If we interrupted in managed code, but it wasn't at a poll point,
      // we need to return back to the thread so it can hit a poll.


      // First, though, we need to wait for the patches to be done.
      // while (!patches_done) {
      // }

      for (auto [start, end] : managed_blob_text_regions) {
        __builtin___clear_cache((char*)start, (char*)end);
      }

      printf(
          "ManagedUntracked: pc:0x%zx sig:%d invl:%d!\n", return_address, sig, invalid_state_abort);
      if (sig == SIGILL) {
        alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_ABORT;
        break;
      }
      if (not invalid_state_abort) {
        invalid_state_abort = true;
        return;
      }
      alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_ABORT;
      invalid_state_abort = false;
      break;

    case StackState::ManagedTracked:
      // it's possible to be at a managed poll point *and* get interrupted
      // through SIGUSR2
      alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_SAFEPOINT;
      break;

    case StackState::Unmanaged:
      assert(sig == SIGUSR2 && "Unmanaged code got into the barrier handler w/ the wrong signal");
      alaska::thread_tracking::my_state.join_status = ALASKA_JOIN_REASON_SIGNAL;
      break;
  }

  invalid_state_abort = false;

  // Conservatively pin this thread's stack + register roots. We deliberately do
  // NOT use the precise, libunwind-based get_pinned_handles here: libunwind is not
  // signal-reentrant -- it holds an internal lock and transiently unblocks signals
  // while unwinding, so a signal landing mid-unwind re-enters this handler and
  // self-deadlocks on that lock (observed as an all-thread hang in
  // get_pinned_handles). The conservative scan is allocation- and unwind-free
  // (exactly like the in-systrap fast path above) and only over-approximates the
  // root set, which is safe for both compaction (handles are relocatable) and the
  // cycle collector (over-pinning only retains garbage, never frees a live object).
  void* sp_lo = (void*)ucontext->uc_mcontext.gregs[REG_RSP];
  // x86-64 SysV red zone: a leaf / non-frame-adjusting function may keep live values --
  // including handles spilled by the compiler -- in the 128 bytes BELOW RSP without moving RSP,
  // and the kernel preserves the interrupted thread's red zone across signal delivery. Scanning
  // only [RSP, stack_top) would miss a handle parked there, so reclaim_dead_handles frees it (it
  // is never marked present) or compaction relocates it (it is never pinned) while the mutator
  // still holds its raw backing pointer -- the residual binarytrees free-list / poison-read
  // crashes. Drop sp_lo by the red zone so the pin scan, present scan, and unpin all cover it.
  // The red zone is within the thread's mapped stack, so this never faults; conservative
  // over-scan only over-retains / over-pins, which is safe for reclaim and compaction alike.
  static constexpr long kRedZone = 128;
  sp_lo = (void*)((char*)sp_lo - kRedZone);
  void* sp_hi = alaska::thread_tracking::my_state.stack_top;
  mark_conservative_range(sp_lo, sp_hi, /*pin*/ true);
  mark_registers_conservative(ucontext, /*pin*/ true);

  // Stackscan: conservatively mark this thread's stack+register handles present
  // (before parking, so all marks are committed before the reclaim callback runs).
  present_scan_self(sp_lo, ucontext, /*from_systrap=*/false);

  // Park until the orchestrator releases the world, then unpin.
  rv_park();
  mark_registers_conservative(ucontext, /*unpin*/ false);
  mark_conservative_range(sp_lo, sp_hi, /*unpin*/ false);

  clear_pending_signals();

  for (auto [start, end] : managed_blob_text_regions) {
    __builtin___clear_cache((char*)start, (char*)end);
  }
}



// Record this thread's stack top (highest address) once, at join, so the barrier
// can conservatively scan an in-systrap thread (see get_conservative_handles).
// Safe to call from the initial (non-signal) join; guarded so the repeated calls
// via clear_pending_signals (which run inside the signal handler) are no-ops and
// never do the /proc-reading pthread_getattr_np from signal context.
static void record_stack_top(void) {
  if (alaska::thread_tracking::my_state.stack_top) return;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
  void *addr = nullptr;
  size_t size = 0;
  if (pthread_attr_getstack(&attr, &addr, &size) == 0 && addr) {
    alaska::thread_tracking::my_state.stack_top = (char*) addr + size;
  }
  pthread_attr_destroy(&attr);
}

static void setup_signal_handlers(void) {
  record_stack_top();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Block signals while we are in these signal handlers.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGILL);
  sigaddset(&sa.sa_mask, SIGUSR2);

  // Store siginfo (ucontext) on the stack of the signal
  // handlers (so we can grab the return address)
  sa.sa_flags = SA_SIGINFO;
  // Go to the `barrier_signal_handler`
  sa.sa_sigaction = alaska_barrier_signal_handler;
  // Attach this action on two signals:
  assert(sigaction(SIGILL, &sa, NULL) == 0);
  assert(sigaction(SIGUSR2, &sa, NULL) == 0);
}


static void clear_pending_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Ignoring a signal while it is pending clears its pending status. Only do this
  // for SIGUSR2 (our barrier signal). Do NOT touch SIGILL: under liballocs it is
  // owned by systrap (our handler is merely chained), so momentarily setting it to
  // SIG_IGN makes liballocs' sigaction interposer drop the chain -- a window in
  // which a concurrent foreign SIGILL aborts. Clearing a pending SIGILL would also
  // discard a real syscall trap. (When liballocs is absent this is still correct:
  // a stray pending SIGILL there would be a genuine fault we should not swallow.)
  sa.sa_handler = SIG_IGN;
  assert(sigaction(SIGUSR2, &sa, NULL) == 0);
  // Now, set the handlers up again.
  setup_signal_handlers();
}




// void alaska::barrier::add_self_thread(void) {
//   setup_signal_handlers();
//   alaska::thread_tracking::join();
//   alaska::thread_tracking::my_state.escaped = 0;
// }

// void alaska::barrier::remove_self_thread(void) { alaska::thread_tracking::leave(); }

// Register the CALLING thread as a barrier participant NOW (install its signal handler, record its
// stack top, add it to the registry). alaska_init calls this on the main thread BEFORE creating the
// periodic barrier thread, closing the startup race where the first 50ms barriers fired while the
// main thread was not yet registered -- so the barrier waited for nobody and compacted/relocated
// the still-running, unpinned main thread's objects (the intermittent binarytrees startup crash).
// Idempotent: ThreadRegistry::join keys by pthread_self() and record_stack_top guards on a set top.
extern "C" void alaska_barrier_register_self(void) { alaska::thread_tracking::join(); }

/**
 * This function parses a stackmap emitted from LLVM and pushes all
 * the patch points to
 */
void parse_stack_map(uint8_t* t) {
  alaska::StackMapParser p(t);

  auto currFunc = p.functions_begin();
  size_t recordCount = 0;

  for (auto it = p.records_begin(); it != p.records_end(); it++) {
    auto record = *it;
    std::uintptr_t addr = currFunc->getFunctionAddress() + record.getInstructionOffset();
    if (++recordCount == currFunc->getRecordCount()) {
      currFunc++;
      recordCount = 0;
    }

    if (record.getID() == 'BLOK') {
      block_rets.add(addr);
    }

    if (record.getID() == 'PATC') {
      // Apply the instruction patch
      auto* rip = (void*)(addr - ALASKA_PATCH_SIZE);

      PatchPoint p;
      p.pc = (inst_t*)rip;


      // X86:
#ifdef __amd64__
      // Byte pointer to the instruction.
      p.inst_sig = 0x0B'0F;
      // A 2 byte nop
      p.inst_nop = 0x90'66;
#endif

// Arm is way easier than x86...
#ifdef __aarch64__
      p.inst_nop = 0xd503201f;  // nop
      p.inst_sig = 0x00000000;  // udf #0
#endif


#ifdef __riscv
      p.inst_nop = 0x0001;  // nop
      p.inst_sig = 0x0000;  // unimp
#endif

      patchPoints.push(p);
    }
    if (record.getID() == 'PATC') {
      addr -= ALASKA_PATCH_SIZE;
    }

    PinSetInfo psi;
    psi.count = 0;

    for (std::uint16_t i = 3; i < record.getNumLocations(); i++) {
      auto l = record.getLocation(i);

      switch (l.getKind()) {
        case alaska::StackMapParser::LocationKind::Direct:
          psi.regNum = l.getDwarfRegNum();
          psi.offset = l.getOffset();
          break;

        case alaska::StackMapParser::LocationKind::Constant:
          psi.count = l.getSmallConstant();
          break;

        default:
          break;
      }
    }


    if (record.getID() == 'HFLT') {
      printf("handle fault at 0x%lx lo:%d, loc:%d\n", addr, record.getNumLiveOuts(),
          record.getNumLocations());
      for (std::uint16_t i = 0; i < record.getNumLocations(); i++) {
        auto l = record.getLocation(i);

        switch (l.getKind()) {
          case alaska::StackMapParser::LocationKind::Direct: {
            auto regNum = l.getDwarfRegNum();
            auto offset = l.getOffset();
            printf(" direct: regnum = %d, offset = %d\n", regNum, offset);
            break;
          }

          case alaska::StackMapParser::LocationKind::Indirect: {
            auto regNum = l.getDwarfRegNum();
            auto offset = l.getOffset();
            printf(" indirect: regnum = %d, offset = %d\n", regNum, offset);
            break;
          }

          case alaska::StackMapParser::LocationKind::Constant: {
            auto constant = l.getSmallConstant();
            printf(" constant = %d\n", constant);
            break;
          }

          default:
            printf(" other\n");
            break;
        }
      }
    }

    if (record.getID() == 'PATC') {
      pin_map[addr] = psi;
      //       if (record.getID() == 'BLOK') {
      // #ifdef __amd64__
      //         pin_map[addr - 5] = psi;  // TODO(HACK)
      // #endif
      // #ifdef __aarch64__
      //         pin_map[addr - 4] = psi;
      // #endif
      //       }
    }
  }


  patchNop();
}



void alaska_blob_init(struct alaska_blob_config* cfg) {
  managed_blob_text_regions.push({cfg->code_start, cfg->code_end});

  // Not particularly safe, but we will ignore that for now.
  auto patch_page = (void*)((uintptr_t)cfg->code_start & ~0xFFF);
  size_t size = round_up(cfg->code_end - cfg->code_start, 4096);
  mprotect(patch_page, size + 4096, PROT_EXEC | PROT_READ | PROT_WRITE);

  if (cfg->stackmap) parse_stack_map((uint8_t*)cfg->stackmap);
}

// This function doesn't really need to exist,
extern "C" void alaska_barrier_poll(void) {
  abort();
  return;
}

extern "C" void alaska_barrier_signal_join(void) {
  alaska_barrier_poll();
}
