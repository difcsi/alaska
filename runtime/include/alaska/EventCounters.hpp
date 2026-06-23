/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Process-global tallies for runtime "events" we want to count across a whole
 * benchmark run: reference-count increments/decrements and GC reclamations (the
 * GC events) and heap compaction passes / objects relocated (the Anchorage
 * events).
 *
 * These counters are MEASUREMENT-ONLY: the increment call sites are compiled in
 * exclusively when ALASKA_ENABLE_EVENT_COUNTERS is set (the `measurement` build
 * preset), so the normal figure-7 timing builds carry no extra instructions on
 * the refcount hot path. Counting itself is just a relaxed atomic add; a counter
 * stays zero for configs whose service is disabled (e.g. incref/decref are never
 * emitted by a noservice build, and compaction never runs without Anchorage).
 *
 * At process exit alaska_events_dump() writes the tallies to the file named by
 * $ALASKA_EVENT_LOG (one `key=value` pair per line), or to stderr if that env
 * var is unset. Alongside the running tallies it also emits a teardown snapshot
 * of the handle table -- the total live handles and how many still hold a
 * nonzero reference count (see HandleTable::census_handles) -- which surfaces
 * refcount leaks / never-freed handles. The figure-7 sweep points
 * $ALASKA_EVENT_LOG at a per-run temp file and folds the parsed counts in as
 * extra per-benchmark metrics.
 */
#pragma once

#include <stdint.h>

namespace alaska::events {

  // The raw counters. Defined once in core/EventCounters.cpp; libalaska's
  // refcount/anchorage code links against them via libalaska_core.
  extern uint64_t g_incref_count;
  extern uint64_t g_decref_count;
  extern uint64_t g_gc_frees;
  extern uint64_t g_compaction_passes;
  extern uint64_t g_objects_moved;
  extern uint64_t g_halloc_count;
  extern uint64_t g_hfree_count;

  // A handle was allocated (halloc/hcalloc). Cumulative allocation traffic --
  // nonzero for any benchmark that uses the heap, unlike the teardown census
  // which only sees handles that survive to process exit.
  static inline void halloc_event(void) {
    __atomic_fetch_add(&g_halloc_count, 1, __ATOMIC_RELAXED);
  }

  // A handle was freed via the program's hfree path (NOT the GC reclaim path,
  // which is tallied separately by gc_free_event).
  static inline void hfree_event(void) {
    __atomic_fetch_add(&g_hfree_count, 1, __ATOMIC_RELAXED);
  }

  // A reference count was incremented on a real handle.
  static inline void inc_refcount_event(void) {
    __atomic_fetch_add(&g_incref_count, 1, __ATOMIC_RELAXED);
  }

  // A reference count was decremented on a real handle.
  static inline void dec_refcount_event(void) {
    __atomic_fetch_add(&g_decref_count, 1, __ATOMIC_RELAXED);
  }

  // The GC reclaimed `n` handles (zero-refcount stackscan reclaim and/or the
  // cycle collector). Pass the count freed in one reclaim batch.
  static inline void gc_free_event(uint64_t n) {
    __atomic_fetch_add(&g_gc_frees, n, __ATOMIC_RELAXED);
  }

  // One heap-compaction pass relocated `moved` objects (only recorded when it
  // actually moved something, so the pass count tracks useful compactions).
  static inline void compaction_event(uint64_t moved) {
    __atomic_fetch_add(&g_compaction_passes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_objects_moved, moved, __ATOMIC_RELAXED);
  }

}  // namespace alaska::events

// Emit the current tallies (see file header for the destination/format). Wired
// to atexit() in alaska_init so every process reports its counts on the way out.
extern "C" void alaska_events_dump(void);
