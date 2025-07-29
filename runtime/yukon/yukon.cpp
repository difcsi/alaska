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

#include <math.h>
#include <dlfcn.h>
#include <alaska/alaska.hpp>
#include <alaska/utils.h>
#include <alaska/Runtime.hpp>
#include <alaska/Configuration.hpp>
#include "alaska.h"
#include "alaska/HugeObjectAllocator.hpp"
#include "alaska/ThreadCache.hpp"
#include <alaska/liballoc.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>
#include <assert.h>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>

#include <yukon/yukon.hpp>
#include <sys/ioctl.h>


#define write_csr(reg, val) \
  ({ asm volatile("csrw %0, %1" ::"i"(reg), "rK"((uint64_t)val) : "memory"); })


#define read_csr(csr, val) \
  __asm__ __volatile__("csrr %0, %1" : "=r"(val) : "n"(csr) : /* clobbers: none */);




static inline uint64_t read_instret() {
  uint64_t instret;
  asm volatile("rdinstret %0" : "=r"(instret));
  return instret;
}

static inline uint64_t read_cycle_counter() {
  uint64_t cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}

static inline uint64_t read_l2_tlb_accesses(void) {
  uint64_t tlb_misses;
  asm volatile("csrr %0, 0xc6" : "=r"(tlb_misses));
  return tlb_misses;
}


#define wait_for_csr_zero(reg)         \
  do {                                 \
    volatile uint32_t csr_value = 0x1; \
    do {                               \
      read_csr(reg, csr_value);        \
    } while (csr_value != 0);          \
  } while (0);


#define CSR_HTBASE 0xc2
#define CSR_HTDUMP 0xc3
#define CSR_HTINVAL 0xc4


static uint64_t instructions_in_runtime = 0;
static uint64_t cycles_in_runtime = 0;

static uint64_t instructions_in_localizer = 0;
static uint64_t cycles_in_localizer = 0;

static thread_local alaska::ThreadCache *tc = NULL;
static alaska::ThreadCache *dump_tc = NULL;
static alaska::Runtime *the_runtime = NULL;
static uint64_t yukon_mean_dump_interval = 10000;  // microseconds


static const char *riscv_abi_names[] = {
    "zero",
    "ra",
    "sp",
    "gp",
    "tp",
    "t0",
    "t1",
    "t2",
    "s0/fp",
    "s1",
    "a0",
    "a1",
    "a2",
    "a3",
    "a4",
    "a5",
    "a6",
    "a7",
    "s2",
    "s3",
    "s4",
    "s5",
    "s6",
    "s7",
    "s8",
    "s9",
    "s10",
    "s11",
    "t3",
    "t4",
    "t5",
    "t6",
};

static void yukon_signal_handler(int sig, siginfo_t *info, void *ucontext) {
  // If a pagefault occurs while handle table walking, we will throw the
  // exception back up and even if you handle the page fault, the HTLB
  // stores the fact that it will cause an exception until you
  // invalidate the entry.
  if (sig == SIGSEGV) {
    // TODO: if the faulting address has the top bit set  (sv39) then we need to
    //       treat that as a page fault to the *handle table*. Basically, we need
    //       to read/write that handle entry.
    // printf("Caught segfault to address %p. Clearing htlb and trying again!\n", info->si_addr);

    uint64_t fault_pc = 0;
#if defined(__riscv)
    ucontext_t *uc = (ucontext_t *)ucontext;
    greg_t *regs = uc->uc_mcontext.__gregs;

    printf("Segfault at address: %p on thread %ld\n", info->si_addr, pthread_self());
    printf("Register state:\n");
    fault_pc = regs[0];
    printf("ra =0x%016lx inst=%08x\n", regs[0], *(uint32_t *)regs[0]);
    printf("sp =0x%016lx\n", regs[1]);
    printf("gp =0x%016lx\n", regs[2]);
    printf("tp =0x%016lx\n", regs[3]);
    for (int i = 1; i < 31; i++) {
      printf("  x%-2d/%-5s =0x%016lx\n", i, riscv_abi_names[i + 1], regs[i]);
    }
#endif

    // get a backtrace
    void *buffer[64];
    int nptrs = backtrace(buffer, sizeof(buffer) / sizeof(void *));
    char **symbols = backtrace_symbols(buffer, nptrs);
    if (symbols == NULL) {
      perror("backtrace_symbols");
      exit(EXIT_FAILURE);
    }
    printf("Backtrace:\n");
    for (int i = 0; i < nptrs; i++) {
      printf("%016lx %s\n", (uint64_t)buffer[i], symbols[i]);
    }
    // free(symbols);

    // FILE *maps_file = fopen("/proc/self/maps", "r");
    // char line[512];
    // while (fgets(line, sizeof(line), maps_file)) {
    //   printf("YUKON_MAP_REGION=%s", line);
    //   // if faultpc is within, print the offset
    //   uintptr_t start, end;
    //   char perms[5];
    //   if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
    //     if (fault_pc >= start && fault_pc < end) {
    //       uintptr_t offset = fault_pc - start;
    //       printf(
    //           "Faulting address %p is within this region, offset %lx\n", (void *)fault_pc, offset);
    //     }
    //   }
    // }
    // fclose(maps_file);


    exit(-11);
    // write_csr(CSR_HTINVAL, ((1LU << (64 - ALASKA_SIZE_BITS)) - 1));
    return;
  }


  // Pause requested.
  if (sig == SIGUSR2) {
    return;
  }
}




static volatile bool delayed_localization_pending = false;
static volatile long localization_latch_depth = 0;
static long localizations = 0;
static alaska::RateCounter immediate_localizations;
static alaska::RateCounter delayed_localizations;




static bool pending_interrupt = false;

// Generate exponentially distributed random number with the given mean
// result is in us for use with itimer
static uint64_t exp_rand(uint64_t mean_us) {
  return mean_us;
  double u = drand48();

  // u = [0,1), uniform random, now convert to exponential
  u = -log(1.0 - u) * ((double)mean_us);

  // now shape u back into a uint64_t and return
  uint64_t ret = 0;
  if (u > ((double)(-1ULL))) {
    ret = -1ULL;
  } else {
    ret = (uint64_t)u;
  }

  // corner case
  if (ret == 0) {
    ret = 1;
  }

  return ret;
}


static size_t localization_attempts = 0;
static bool enable_localization = true;


static void schedule_localization(uint64_t interval_override = 0) {
  if (not enable_localization) return;

  uint64_t interval =
      interval_override != 0 ? interval_override : exp_rand(yukon_mean_dump_interval);

  // Apply a minimum interval for safety (or something)
  constexpr uint64_t min_interval = 20;
  if (interval < min_interval) interval = min_interval;

  struct yukon_schedule_arg arg;
  arg.delay = interval;
  arg.handler_address = (unsigned long)yukon::dump_alarm_handler;
  long res = ioctl(alaska::HandleTable::get_ht_fd(), YUKON_IOCTL_SCHEDULE, &arg);
  pending_interrupt = true;
  if (res < 0) {
    printf("fd = %d\n", alaska::HandleTable::get_ht_fd());
    perror("Failed to schedule localization");
    exit(-1);
  }

  // // schedule the next dump in the future!
  // if ((long)ualarm(interval, 0) == -1) {
  //   perror("Failed to setup ualarm for dumping");
  //   exit(-1);
  // }
}




extern "C" void yukon_enable_localization(int enable) {
  if (getenv("NODUMP") != nullptr) {
    fprintf(stderr, "YUKON: localization disabled by NODUMP env var!\n");
    enable = 0;
  }

  enable_localization = enable;
  if (enable_localization) {
    schedule_localization();
  }
}


static bool attempt_localization(void) {
  if (localization_latch_depth > 0) {
    return false;
  }

  if (not enable_localization) {
    //
    return true;
  }

  auto start_cycle = read_cycle_counter();
  auto start_insts = read_instret();
  // Grab the thread cache
  auto *tc = yukon::get_dump_tc();
  // And trigger an HTLB dump

  yukon::dump_htlb(tc);
  localizations++;

  instructions_in_localizer += read_instret() - start_insts;
  cycles_in_localizer += read_cycle_counter() - start_cycle;

  return true;
}



void yukon::dump_alarm_handler(int sig) {
  pending_interrupt = false;
  if (likely(alaska::is_initialized())) {
    if (attempt_localization()) {
      // if localization succeeded, track it
      immediate_localizations.track();
      // Then, schedule the next one
      schedule_localization();
    } else {
      delayed_localization_pending = true;
    }
  } else {
    printf("Not initialized!\n");
    schedule_localization();
  }
}

struct LocalizationLatch {
  LocalizationLatch() { localization_latch_depth++; }

  ~LocalizationLatch() {
    localization_latch_depth--;
    if (delayed_localization_pending) {
      if (attempt_localization()) {
        // if it didn't succeed, we scheduled a delayed one
        // and should track that.
        delayed_localizations.track();
        delayed_localization_pending = false;
        schedule_localization();
      }
    }
  }
};

struct RuntimeTracker {
  uint64_t start_inst;
  uint64_t start_cycle;
  RuntimeTracker() {
    start_inst = read_instret();
    start_cycle = read_cycle_counter();
  }
  ~RuntimeTracker() {
    __asm__ volatile("fence" ::: "memory");
    instructions_in_runtime += (read_instret() - start_inst);
    cycles_in_runtime += (read_cycle_counter() - start_cycle);
  }
};


pthread_t dump_thread;

static void setup_signal_handlers(void);



#define ALASKA_THREAD_TRACK_STATE_T int
#define ALASKA_THREAD_TRACK_INIT setup_signal_handlers();
#include <alaska/thread_tracking.in.hpp>


////////////////////////////////////////////

static pthread_barrier_t the_barrier;
static long barrier_last_num_threads = 0;
static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;




static void participant_join(bool leader) {
  // Wait on the barrier so everyone's state has been commited.
  if (alaska::thread_tracking::threads().num_threads() > 1) {
    pthread_barrier_wait(&the_barrier);
  }
}




static void participant_leave(bool leader) {
  // wait for the the leader (and everyone else to catch up)
  if (alaska::thread_tracking::threads().num_threads() > 1) {
    pthread_barrier_wait(&the_barrier);
  }
}

static void clear_pending_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Ignoring a signal while it is pending will clear it's pending status
  sa.sa_handler = SIG_IGN;
  assert(sigaction(SIGUSR2, &sa, NULL) == 0);
  // Now, set them up again
  setup_signal_handlers();
}

struct YukonBarrierManager : public alaska::BarrierManager {
  ~YukonBarrierManager() override = default;
  bool begin(void) override {
    // Take locks so nobody else tries to signal a barrier.
    pthread_mutex_lock(&barrier_lock);
    alaska::thread_tracking::threads().lock_thread_creation();

    auto num_threads = alaska::thread_tracking::threads().num_threads();

    // If the barrier needs resizing, do so.
    if (barrier_last_num_threads != num_threads) {
      if (barrier_last_num_threads != 0) pthread_barrier_destroy(&the_barrier);
      // Initialize the barrier so we know when everyone is ready!
      pthread_barrier_init(&the_barrier, NULL, num_threads);
      barrier_last_num_threads = num_threads;
    }

    int retries = 0;
    int signals_sent = 0;

    bool success = true;

    printf("Thread %ld beginning barrier with %ld threads\n", pthread_self(), num_threads);

    alaska::thread_tracking::threads().for_each_locked([&](auto thread, auto *state) {
      if (thread == pthread_self()) {
        // Don't signal ourselves, we are the orchestrator.
        return;
      }
      pthread_kill(thread, SIGUSR2);
    });


    participant_join(true);


    return success;
  }
  void end(void) override {
    // ck::set<void*> locked;
    // Join the barrier to signal everyone we are done.
    participant_leave(true);

    // Unlock all the locks we took.
    alaska::thread_tracking::threads().unlock_thread_creation();
    pthread_mutex_unlock(&barrier_lock);
  }
};



static YukonBarrierManager the_barrier_manager;

static void alaska_barrier_signal_handler(int sig, siginfo_t *info, void *ptr) {
  ucontext_t *ucontext = (ucontext_t *)ptr;
  uintptr_t return_address = 0;

#if defined(__amd64__)
  return_address = ucontext->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  return_address = ucontext->uc_mcontext.pc;
#elif defined(__riscv)
  return_address = ucontext->uc_mcontext.__gregs[REG_PC];
#endif

  // Simply join the barrier, then leave immediately. This
  // will deal with all the synchronization that needs done.
  participant_join(false);

  participant_leave(false);

  clear_pending_signals();
}


static void setup_signal_handlers(void) {
  unsetenv("LD_PRELOAD");
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Block signals while we are in these signal handlers.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGUSR2);

  // Store siginfo (ucontext) on the stack of the signal
  // handlers (so we can grab the return address)
  sa.sa_flags = SA_SIGINFO;
  // Go to the `barrier_signal_handler`
  sa.sa_sigaction = yukon_signal_handler;
  // Attach this action on two signals:
  assert(sigaction(SIGSEGV, &sa, NULL) == 0);

  sa.sa_sigaction = alaska_barrier_signal_handler;
  assert(sigaction(SIGUSR2, &sa, NULL) == 0);
  // assert(sigaction(SIGUSR2, &sa, NULL) == 0);
}
///////////////////////////////////////////



static unsigned long last_dump_cycle = 0;

static uint64_t last_dump_instret = 0;
static uint64_t last_dump_misses = 0;

namespace yukon {
  void set_handle_table_base(void *addr) {
    uint64_t value;
    read_csr(CSR_HTBASE, value);
    if (getenv("YUKON_PHYS") != nullptr) {
      value |= (1LU << 63);
    }

    write_csr(CSR_HTBASE, value);
  }




  void dump_htlb(alaska::ThreadCache *tc) {
    auto fd = alaska::HandleTable::get_ht_fd();
    static size_t localization_count = 0;

    auto &rt = alaska::Runtime::get();

    // The size of the HTLB
    size_t size = 16 + 512;
    auto *space = tc->localizer.get_hotness_buffer(size);
    memset(space, 0, size * sizeof(alaska::handle_id_t));
    int r = read(fd, space, size * sizeof(alaska::handle_id_t));
    tc->localizer.feed_hotness_buffer(size, space);
  }

  alaska::ThreadCache *get_tc() {
    if (the_runtime == NULL) init();
    if (tc == NULL) tc = the_runtime->new_threadcache();
    return tc;
  }

  alaska::ThreadCache *get_dump_tc() {
    if (the_runtime == NULL) init();
    if (dump_tc == NULL) dump_tc = the_runtime->new_threadcache();
    return dump_tc;
  }


  static char stdout_buf[BUFSIZ];
  static char stderr_buf[BUFSIZ];
  void init(void) {
    // __asm__ volatile("addi x0, x1, 0");
    setvbuf(stdout, stdout_buf, _IOLBF, BUFSIZ);
    setvbuf(stderr, stderr_buf, _IOLBF, BUFSIZ);

    alaska::Configuration config;
    config.huge_strategy = alaska::HugeAllocationStrategy::MALLOC_BACKED;

    the_runtime = new alaska::Runtime(config);
    // void *handle_table_base = the_runtime->handle_table.get_base();
    // printf("Handle table at %p\n", handle_table_base);
    // Make sure the handle table performs mlocks
    the_runtime->handle_table.enable_mlock();
    the_runtime->barrier_manager = &the_barrier_manager;
    atexit([]() {
      alaska::printf("YUKON_RTINST=%zu\n", instructions_in_runtime);
      alaska::printf("YUKON_INST_LOC=%zu\n", instructions_in_localizer);
      alaska::printf("YUKON_CYCLE_LOC=%zu\n", cycles_in_localizer);
    });


    if (getenv("YUKON_PHYS") != nullptr) {
      printf("Setting up handles to bypass the TLB when they're cached!\n");
      uint64_t value;
      read_csr(CSR_HTBASE, value);
      value |= (1LU << 63);
      write_csr(CSR_HTBASE, value);
    }



    signal(SIGPROF, yukon::dump_alarm_handler);
    yukon_mean_dump_interval = 10 * 1000;
    if (getenv("NODUMP") == nullptr) {
      fprintf(stderr, "YUKON: dump interval %zuus\n", yukon_mean_dump_interval);

      yukon_enable_localization(true);
      // Schedule the first dump for 50ms from now (just to make sure
      // initialization is done. This is a Super-Hack)
      schedule_localization(50 * 1000);
    } else {
      yukon_enable_localization(false);
      enable_localization = false;
      yukon_mean_dump_interval = 0;
      fprintf(stderr, "YUKON: localization disabled!\n");
    }
  }
}  // namespace yukon



void __attribute__((constructor(102))) alaska_init(void) {
  unsetenv("LD_PRELOAD");  // make it so we don't run alaska in subprocesses!
  // allocate
  yukon::get_tc();
}


static void touch_pages(uintptr_t start, uintptr_t end) {
  constexpr uintptr_t page_size = 4096;
  // Align the start to the beginning of the first 4KB page in the range
  uintptr_t current_page = start & ~(page_size - 1);
  if (current_page < start) {
    current_page += page_size;
  }

  // Loop through each page until we exceed the end
  while (current_page < end) {
    volatile uint8_t *p = (volatile uint8_t *)current_page;
    *p = 0;
    // Move to the next page
    current_page += page_size;
  }
}

static void *_halloc(size_t sz, int zero) {
  void *result = NULL;

  result = yukon::get_tc()->halloc(sz, zero);
  // if (m) {
  //   auto backing_data = (uintptr_t)m->get_pointer();
  //   touch_pages(backing_data, backing_data + sz);
  // }
  if (result == NULL) errno = ENOMEM;

  return result;
}

extern "C" void *halloc(size_t sz) noexcept {
  LocalizationLatch loc_latch;
  // RuntimeTracker fencer;
  return _halloc(sz, 0);
}
extern "C" void *hcalloc(size_t nmemb, size_t size) {
  LocalizationLatch loc_latch;
  // RuntimeTracker fencer;
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
extern "C" void *hrealloc(void *handle, size_t new_size) {
  LocalizationLatch loc_latch;
  // RuntimeTracker fencer;

  alaska::LockedThreadCache tc = *yukon::get_tc();

  // If the handle is null, then this call is equivalent to malloc(size)
  if (handle == NULL) {
    return tc->halloc(new_size);
  }
  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m == NULL) {
    if (!alaska::Runtime::get().heap.huge_allocator.owns(handle)) {
      log_fatal("realloc edge case: not a handle %p!", handle);
      exit(-1);
    }
  }

  // If the size is equal to zero, and the handle is not null, realloc acts like free(handle)
  if (new_size == 0) {
    log_debug("realloc edge case: zero size %p!", handle);
    // If it wasn't a handle, just forward to the system realloc
    tc->hfree(handle);
    return NULL;
  }

  handle = tc->hrealloc(handle, new_size);
  return handle;
}



extern "C" void hfree(void *ptr) {
  LocalizationLatch loc_latch;
  // RuntimeTracker fencer;
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;


  alaska::LockedThreadCache tc = *yukon::get_tc();
  tc->hfree(ptr);
}


extern "C" size_t halloc_usable_size(void *handle) {
  alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
  if (m == nullptr) {
    // slow fallback
    LocalizationLatch loc_latch;
    alaska::LockedThreadCache tc = *yukon::get_tc();
    return tc->get_size(handle);
  }
  if (m->is_free()) return 0;
  void *ptr = m->get_pointer();
  auto header = alaska::ObjectHeader::from(ptr);
  return header->object_size();
}



void *operator new(size_t size) { return halloc(size); }
void *operator new[](size_t size) { return halloc(size); }
void operator delete(void *ptr) { hfree(ptr); }
void operator delete[](void *ptr) { hfree(ptr); }


extern "C" {
void *malloc(size_t size) { return halloc(size); }
void *calloc(size_t size, size_t count) { return hcalloc(size, count); }
void *realloc(void *ptr, size_t newsize) { return hrealloc(ptr, newsize); }
void free(void *ptr) { hfree(ptr); }
size_t malloc_usable_size(void *ptr) { return halloc_usable_size(ptr); }
}
