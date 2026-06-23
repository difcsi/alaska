/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Storage for the runtime event counters and the exit-time dump that the
 * figure-7 sweep consumes. See alaska/EventCounters.hpp.
 */

#include <alaska/EventCounters.hpp>
#include <alaska/Runtime.hpp>
#include <stdio.h>
#include <stdlib.h>

namespace alaska::events {
  uint64_t g_incref_count = 0;
  uint64_t g_decref_count = 0;
  uint64_t g_gc_frees = 0;
  uint64_t g_compaction_passes = 0;
  uint64_t g_objects_moved = 0;
  uint64_t g_halloc_count = 0;
  uint64_t g_hfree_count = 0;
}  // namespace alaska::events

extern "C" void alaska_events_dump(void) {
  using namespace alaska::events;
  unsigned long incref = (unsigned long)__atomic_load_n(&g_incref_count, __ATOMIC_RELAXED);
  unsigned long decref = (unsigned long)__atomic_load_n(&g_decref_count, __ATOMIC_RELAXED);
  unsigned long gc_frees = (unsigned long)__atomic_load_n(&g_gc_frees, __ATOMIC_RELAXED);
  unsigned long passes = (unsigned long)__atomic_load_n(&g_compaction_passes, __ATOMIC_RELAXED);
  unsigned long moved = (unsigned long)__atomic_load_n(&g_objects_moved, __ATOMIC_RELAXED);
  unsigned long halloc = (unsigned long)__atomic_load_n(&g_halloc_count, __ATOMIC_RELAXED);
  unsigned long hfree = (unsigned long)__atomic_load_n(&g_hfree_count, __ATOMIC_RELAXED);

  // Teardown snapshot of the handle table: how many handles still exist and how
  // many of those still hold a nonzero reference count. The barrier thread has
  // already been quiesced (this dump runs last in the atexit LIFO), so the table
  // is not mutating underneath the walk.
  unsigned long handles_total = 0;
  unsigned long handles_nonzero_rc = 0;
  if (auto *rt = alaska::Runtime::get_ptr()) {
    auto census = rt->handle_table.census_handles();
    handles_total = (unsigned long)census.total;
    handles_nonzero_rc = (unsigned long)census.nonzero_refcount;
  }

  // With $ALASKA_EVENT_LOG set, write a clean machine-readable file (for the
  // harness). Otherwise fall back to stderr so an interactive run still shows
  // the tally without polluting any results file.
  const char *path = getenv("ALASKA_EVENT_LOG");
  FILE *f = stderr;
  bool to_file = false;
  if (path != nullptr && path[0] != '\0') {
    FILE *opened = fopen(path, "w");
    if (opened != nullptr) {
      f = opened;
      to_file = true;
    }
  }

  fprintf(f,
      "halloc=%lu\nhfree=%lu\n"
      "incref=%lu\ndecref=%lu\ngc_frees=%lu\ncompactions=%lu\nobjects_moved=%lu\n"
      "handles_total=%lu\nhandles_nonzero_rc=%lu\n",
      halloc, hfree, incref, decref, gc_frees, passes, moved,
      handles_total, handles_nonzero_rc);

  if (to_file) {
    fclose(f);
  } else {
    fflush(f);
  }
}
