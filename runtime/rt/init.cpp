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

#if ALASKA_ENABLE_REFCOUNT
// Stackscan reclamation, run inside the barrier (rt/refcount.cpp); present-bitmap
// control (rt/gc_bitmaps.cpp) + the in-barrier present-scan flag (rt/barrier.cpp).
namespace alaska { size_t reclaim_dead_handles(alaska::ThreadCache &tc); }
extern "C" void alaska_gc_present_clear(void);
extern "C" void alaska_gc_present_scan_set(int on);
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

#if ALASKA_ENABLE_REFCOUNT
    // Stackscan: arm the in-barrier conservative present-scan and start from a
    // clean present bitmap, so each participating thread marks its stack handles
    // present during the barrier below. (with_barrier throttles actual barriers to
    // its min interval, so most of these are cheap no-ops.) RECLAIM cadence can be
    // tuned by gating this on `tick`; for now reclamation runs each barrier.
    alaska_gc_present_clear();
    alaska_gc_present_scan_set(1);
#endif

    rt.with_barrier([&]() {
#if ALASKA_ENABLE_CYCLE_COLLECTION
      // Heap compaction and cycle collection are duals (Deutsch & Bobrow): both
      // walk the object graph with the world stopped, so Anchorage does them in
      // the same barrier. Collect cycles less often than we compact -- tracing
      // is more expensive and only worthwhile once candidates have accumulated.
      if (tick % 20 == 0 && rt.cycle_collector.candidate_count() > 0) {
        rt.cycle_collector.collect(*tc);
      }
#endif
#if ALASKA_ENABLE_ANCHORAGE
      rt.heap.compact_sizedpages();
#endif
#if ALASKA_ENABLE_REFCOUNT
      // Stackscan: free zero-refcount handles not marked present (not on any
      // thread's stack). World stopped + nullcount_lock pre-held by with_barrier.
      alaska::reclaim_dead_handles(*tc);
#endif
    });

#if ALASKA_ENABLE_REFCOUNT
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
}
#endif  // ALASKA_BARRIER_THREAD_ENABLED

void __attribute__((constructor(102))) alaska_init(void) {
  // Allocate the runtime simply by creating a new instance of it. Everywhere
  // we use it, we will use alaska::Runtime::get() to get the singleton instance.
  the_runtime = new alaska::Runtime();
  // Attach the runtime's barrier manager
  the_runtime->barrier_manager = &the_barrier_manager;
#if ALASKA_ENABLE_EVENT_COUNTERS
  // Dump the measurement counters on the way out. Registered first so it runs
  // LAST (atexit is LIFO) -- after alaska_stop_barrier_thread below has quiesced
  // the periodic barrier, so no compaction/reclaim updates the counts mid-dump.
  atexit(alaska_events_dump);
#endif
#if ALASKA_BARRIER_THREAD_ENABLED
  pthread_create(&barrier_thread, NULL, barrier_thread_func, NULL);
  atexit(alaska_stop_barrier_thread);
#endif
}

void __attribute__((destructor)) alaska_deinit(void) {}
