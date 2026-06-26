/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */


// This file contains the initialization and deinitialization functions for the
// alaska::Runtime instance, as well as some other bookkeeping logic.

#include <alaska/rt.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/alaska.hpp>
#include <alaska/rt/barrier.hpp>
#include <alaska/EventCounters.hpp>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ck/queue.h>


static alaska::Runtime *the_runtime = nullptr;


struct CompilerRuntimeBarrierManager : public alaska::BarrierManager {
  ~CompilerRuntimeBarrierManager() override = default;
  bool begin(void) override { return alaska::barrier::begin(); }
  void end(void) override { alaska::barrier::end(); }
};

static CompilerRuntimeBarrierManager the_barrier_manager;

extern "C" void alaska_dump(void) { the_runtime->dump(stderr); }


// Defined in halloc.cpp -- this (anchorage) thread's raw thread cache.
extern alaska::ThreadCache *get_tc_r(void);

// Defined in barrier.cpp -- register the calling thread as a barrier participant (handler + stack
// top + registry). Called on the main thread before the periodic barrier thread is created.
extern "C" void alaska_barrier_register_self(void);

#if ALASKA_ENABLE_REFCOUNT
// Stackscan reclamation, run inside the barrier (rt/refcount.cpp); present-bitmap
// control (rt/gc_bitmaps.cpp) + the in-barrier present-scan flag (rt/barrier.cpp).
namespace alaska { size_t reclaim_dead_handles(alaska::ThreadCache &tc); }
extern "C" void alaska_gc_present_clear(void);
extern "C" void alaska_gc_present_scan_set(int on);
// Mark handle roots held in the main image's globals (data+BSS) present. Globals are a root
// class the per-thread stack/register scan never covers; without this a global-rooted handle is
// reclaimed. Run once per barrier before reclaim (see barrier.cpp).
extern "C" void alaska_gc_scan_globals_present(void);
#endif

// The periodic barrier thread only exists when some service does work inside the
// stop-the-world barrier: refcount reclamation (also covers cycle collection,
// which requires refcount) and/or Anchorage heap compaction. A pure noservice
// build spawns no thread at all.
#define ALASKA_BARRIER_THREAD_ENABLED (ALASKA_ENABLE_REFCOUNT || ALASKA_ENABLE_ANCHORAGE)

#if ALASKA_BARRIER_THREAD_ENABLED
static pthread_t barrier_thread;
// Set once at process shutdown (from an atexit handler, which runs before
// _dl_fini/destructors). The barrier thread must stop signalling/barriering
// before any library teardown begins: otherwise it keeps firing SIGUSR2 at
// threads that are already inside exit handlers (e.g. another runtime joining
// its own GC thread), and the nested barrier handler runs against half-torn-down
// state. Plain volatile sig_atomic_t is enough -- single writer, single reader.
static volatile sig_atomic_t barrier_thread_should_stop = 0;
static void *barrier_thread_func(void *) {
  // Make sure this thread owns a thread cache before it ever enters a barrier:
  // lazily creating one needs locks that with_barrier already holds. The cycle
  // collector also uses it to size and free reclaimed objects.
  auto *tc = get_tc_r();
  (void)tc;

  unsigned long tick = 0;
  while (!barrier_thread_should_stop) {
    usleep(50 * 1000);
    if (barrier_thread_should_stop) break;
    auto &rt = alaska::Runtime::get();

#if ALASKA_ENABLE_CYCLE_COLLECTION
    // Stackscan: arm the in-barrier conservative present-scan and start from a
    // clean present bitmap, so each participating thread marks its stack handles
    // present during the barrier below. (with_barrier throttles actual barriers to
    // its min interval, so most of these are cheap no-ops.) RECLAIM cadence can be
    // tuned by gating this on `tick`; for now reclamation runs each barrier.
    // Gated on the GC macro (not ALASKA_ENABLE_REFCOUNT) because the present marks
    // it produces are consumed solely by reclaim_dead_handles below; a plain
    // refcount build runs no reclaim, so it must not pay for the scan either.
    alaska_gc_present_clear();
    alaska_gc_present_scan_set(1);
#endif

    rt.with_barrier([&]() {
#if ALASKA_ENABLE_REFCOUNT
      // Apply every thread's deferred reference-count increments FIRST -- before dec-on-free
      // and before any reclaim/cycle decision below. The Levanoni-Petrank INV-LIVE linchpin: a
      // child carrying a pending (logged) inc must be back at full count before drain_deferred's
      // dec-on-free or reclaim_dead_handles reads it, or a still-live handle could be
      // undercounted to 0 and freed. World stopped + all tc locks held, so every committed log
      // entry is visible; a no-op unless ALASKA_DEFER_RC is set.
      for (auto *tcx : rt.tcs)
        tcx->drain_inc();
      // Drain program-deferred frees first (deferred dec-on-free is the default). For every handle the
      // mutators queued via ThreadCache::defer_free, run the dec-children scan + backing
      // free here, with the world stopped and all tc locks held by with_barrier -- the same
      // invariant reclaim_dead_handles relies on. Doing it BEFORE reclaim lets children
      // decremented to 0 here be reclaimed this same cycle. Iterating rt.tcs is safe:
      // lock_all_thread_caches() holds tcs_lock for the whole callback. A no-op (empty
      // queues) unless deferred mode is enabled.
      for (auto *tcx : rt.tcs)
        tcx->drain_deferred();
      // Roll the handle-slot quarantine AFTER every queue has drained: release the slots
      // withheld a full epoch ago and promote this epoch's. Draining first guarantees any
      // parent that still referenced a to-be-released slot has already run dec-on-free and
      // skipped it (its backing is null), so recycling the slot now is safe. See
      // ThreadCache::quarantine_rotate / hfree_impl (the binarytrees dec-on-free reuse race).
      for (auto *tcx : rt.tcs)
        tcx->quarantine_rotate();
      // Flush every thread's Perceus reuse-cache stash BEFORE the heap walks below. A stashed
      // entry keeps its mapping pointing at a backing that is still marked allocated in its page
      // but is held out of the freelist (reuse limbo); compaction would then relocate/free that
      // backing while reuse still hands out the stale old address -- corrupting the backing and
      // handle freelists (observed as a 100% SIGSEGV in ShardedFreeList::pop with anchorage on).
      // A stash is meant to live only until the freeing thread's next alloc, so flushing it at the
      // barrier almost never discards a useful entry. World-stopped, all tc locks held.
      for (auto *tcx : rt.tcs)
        tcx->flush_reuse_cache();
#endif
      // DIAGNOSTIC subsystem kill-switches (read once). Isolate which world-stopped GC subsystem
      // is the source of a premature free: turn each off independently and see which makes the
      // crash vanish. ALASKA_NO_GC_FREE gates BOTH the cycle collector here and reclaim_dead_handles
      // (refcount.cpp); ALASKA_NO_COMPACT gates compaction.
      static int no_gc_free = -1, no_compact = -1;
      if (no_gc_free < 0) no_gc_free = (getenv("ALASKA_NO_GC_FREE") != nullptr) ? 1 : 0;
      if (no_compact < 0) no_compact = (getenv("ALASKA_NO_COMPACT") != nullptr) ? 1 : 0;
#if ALASKA_ENABLE_CYCLE_COLLECTION
      // Heap compaction and cycle collection are duals (Deutsch & Bobrow): both
      // walk the object graph with the world stopped, so Anchorage does them in
      // the same barrier. Collect cycles less often than we compact -- tracing
      // is more expensive and only worthwhile once candidates have accumulated.
      if (!no_gc_free && tick % 20 == 0 && rt.cycle_collector.candidate_count() > 0) {
        rt.cycle_collector.collect(*tc);
      }
#endif
#if ALASKA_ENABLE_ANCHORAGE
      if (!no_compact) rt.heap.compact_sizedpages();
#endif
#if ALASKA_ENABLE_CYCLE_COLLECTION
      // Stackscan: free zero-refcount handles not marked present (not on any
      // thread's stack). World stopped; the zero-refcount set is a lock-free bitmap.
      // This is the zero-refcount GARBAGE COLLECTOR, so it is gated on the GC macro
      // (ALASKA_ENABLE_CYCLE_COLLECTION), NOT ALASKA_ENABLE_REFCOUNT: a plain
      // refcount / refcount-anchorage build does pure reference counting and never
      // reclaims here, so it also skips maintaining the nullcount map (see
      // alaska_inc_refcount/alaska_dec_refcount).
      //
      // First add the globals to the present set: each participant marked its own stack +
      // registers when it parked, but no one scans the data/BSS segment, so a handle rooted only
      // in a global would be reclaimed here. World-stopped, so these marks are committed before
      // the scan below (and present was cleared at the top of this tick).
      alaska_gc_scan_globals_present();
      alaska::reclaim_dead_handles(*tc);
#endif
    });

#if ALASKA_ENABLE_CYCLE_COLLECTION
    // Disarm the present-scan (paired with the arm above); GC-gated for the same
    // reason -- it only matters for the stackscan reclaim.
    alaska_gc_present_scan_set(0);
#endif
    tick++;
  }

  return NULL;
}

// Runs at the very start of normal process shutdown (atexit, before _dl_fini).
// Quiesce the periodic barrier so no compaction barrier / SIGUSR2 traffic
// overlaps library teardown. The thread observes the flag on its next wakeup
// (<=50ms); if it happens to be mid-barrier, the join below still completes
// cleanly because this thread can service the in-flight SIGUSR2 (runtime state
// is still intact here -- destructors have not run yet).
static void alaska_stop_barrier_thread(void) {
  barrier_thread_should_stop = 1;
  pthread_join(barrier_thread, NULL);
#if ALASKA_ENABLE_REFCOUNT
  // Final flush of the deferred-RC increment logs now that the periodic barrier is gone, so the
  // exit-time event dump / handle census reflect every increment -- otherwise the last <=1 epoch
  // of logged-but-undrained increments would undercount refcounts in deferred mode (skewing
  // handles_nonzero_rc and incref). Best-effort: at process shutdown this thread is effectively
  // alone (same reasoning as the quiesce above) and add_refcount is atomic. A no-op in eager
  // mode (every inc_log is empty).
  auto &rt = alaska::Runtime::get();
  for (auto *tcx : rt.tcs)
    tcx->drain_inc();
#endif
}
#endif  // ALASKA_BARRIER_THREAD_ENABLED

void __attribute__((constructor(102))) alaska_init(void) {
  // Allocate the runtime simply by creating a new instance of it. Everywhere
  // we use it, we will use alaska::Runtime::get() to get the singleton instance.
  the_runtime = new alaska::Runtime();
  // Attach the runtime's barrier manager
  the_runtime->barrier_manager = &the_barrier_manager;
#if ALASKA_ENABLE_CACHE_PROBE
  // Allocate the cache-probe state now that the handle table (and its fixed base
  // address) exists. The probe fires from Mapping::inc/dec_refcount thereafter.
  alaska::events::cache_probe_init();
#endif
#if ALASKA_ENABLE_EVENT_COUNTERS
  // Dump the measurement counters on the way out. Registered first so it runs
  // LAST (atexit is LIFO) -- after alaska_stop_barrier_thread below has quiesced
  // the periodic barrier, so no compaction/reclaim updates the counts mid-dump.
  atexit(alaska_events_dump);
#endif
#if ALASKA_BARRIER_THREAD_ENABLED
  // Register THIS (main) thread as a barrier participant BEFORE the periodic barrier thread exists.
  // Otherwise the first 50ms ticks can fire while the main thread's own registration constructor
  // hasn't run yet (num_threads==1), so the barrier waits for nobody and compaction relocates the
  // running, unpinned main thread's objects -- the intermittent startup binarytrees crash.
  alaska_barrier_register_self();
  pthread_create(&barrier_thread, NULL, barrier_thread_func, NULL);
  atexit(alaska_stop_barrier_thread);
#endif
}

void __attribute__((destructor)) alaska_deinit(void) {}
