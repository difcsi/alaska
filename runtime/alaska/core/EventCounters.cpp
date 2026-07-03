/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Storage for the runtime event counters and the exit-time dump that the
 * figure-7 sweep consumes. See alaska/EventCounters.hpp.
 */

#include <alaska/EventCounters.hpp>
#include <alaska/core/Runtime.hpp>
#include <stdio.h>
#include <stdlib.h>

#if ALASKA_ENABLE_CACHE_PROBE
#include <sys/mman.h>
#endif

namespace alaska::events {
  uint64_t g_incref_count = 0;
  uint64_t g_decref_count = 0;
  uint64_t g_gc_frees = 0;
  uint64_t g_compaction_passes = 0;
  uint64_t g_objects_moved = 0;
  uint64_t g_halloc_count = 0;
  uint64_t g_hfree_count = 0;

  // Deferred reference counting (Levanoni-Petrank). See EventCounters.hpp.
  uint64_t g_rc_deferred = 0;
  uint64_t g_rc_applied = 0;
  uint64_t g_rc_coalesced = 0;
  uint64_t g_rc_overflow = 0;
  uint64_t g_rc_self_flushes = 0;
  uint64_t g_rc_log_hwm = 0;

#if ALASKA_ENABLE_CACHE_PROBE
  // --- Cache-miss characterization probe state ---------------------------------
  // Tallies reported at exit. Non-atomic: this build is for single-mutator
  // characterization runs (e.g. binarytrees), and the barrier thread's occasional
  // dec only perturbs these by a negligible, documented amount.
  static uint64_t g_distinct_lines = 0;     // distinct Mapping cache lines touched
  static uint64_t g_total_line_touches = 0; // every probe call
  static uint64_t g_probe_hits = 0;         // simulated direct-mapped cache hits
  static uint64_t g_probe_misses = 0;       // ... and misses
  static uint64_t g_probe_oob = 0;          // touches outside the bitmap span

  // Simulated direct-mapped cache: one tag (absolute line address) per set. A tag
  // of 0 means empty -- a real Mapping line address is never 0 (the table sits at a
  // high fixed base), so the empty slot reads correctly as a compulsory miss.
  static uint64_t *g_probe_tags = nullptr;
  static uint64_t g_probe_sets = 0;       // power of two
  static uint64_t g_probe_sets_mask = 0;

  // Distinct-line bitmap, indexed by (addr - base) >> 6 over a generous span of the
  // handle table's (fixed-base, grows-in-place) address range.
  static uint8_t *g_probe_bitmap = nullptr;
  static uintptr_t g_probe_base = 0;
  static uint64_t g_probe_span_lines = 0;

  static uint64_t round_down_pow2(uint64_t v) {
    uint64_t p = 1;
    while (p * 2 <= v) p *= 2;
    return p;
  }

  void cache_probe_init(void) {
    // Modeled-footprint set count. Sweeping ALASKA_PROBE_SETS and watching where the
    // simulated miss rate inflects reveals the Mapping-line reuse-distance cliff.
    uint64_t sets = 4096;  // default: 4096 lines = 256 KiB of modeled footprint
    if (const char *e = getenv("ALASKA_PROBE_SETS")) {
      uint64_t v = strtoull(e, nullptr, 0);
      if (v >= 1) sets = v;
    }
    sets = round_down_pow2(sets);
    g_probe_sets = sets;
    g_probe_sets_mask = sets - 1;
    void *tags = mmap(nullptr, sets * sizeof(uint64_t), PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);  // demand-zeroed -> all sets empty
    g_probe_tags = (tags == MAP_FAILED) ? nullptr : (uint64_t *)tags;

    // Distinct-line bitmap. Default covers 4 GiB of handle-table address space
    // (4 GiB >> 6 lines / 8 = 8 MiB of bitmap, demand-paged so only touched pages
    // are resident). Override with ALASKA_PROBE_SPAN_GB if a run grows past it.
    uint64_t span_bytes = 4ULL << 30;
    if (const char *e = getenv("ALASKA_PROBE_SPAN_GB")) {
      uint64_t gb = strtoull(e, nullptr, 0);
      if (gb >= 1) span_bytes = gb << 30;
    }
    g_probe_span_lines = span_bytes >> 6;
    uint64_t bitmap_bytes = (g_probe_span_lines + 7) / 8;
    void *bm = mmap(nullptr, bitmap_bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_probe_bitmap = (bm == MAP_FAILED) ? nullptr : (uint8_t *)bm;

    if (auto *rt = alaska::Runtime::get_ptr()) {
      g_probe_base = (uintptr_t)rt->handle_table.get_base();
    }
  }

  void probe_mapping_line(uintptr_t addr) {
    if (g_probe_tags == nullptr) return;  // init failed / not yet initialized
    uint64_t line = addr >> 6;
    g_total_line_touches++;

    // (a) simulated direct-mapped cache -- works on absolute line address.
    uint64_t set = line & g_probe_sets_mask;
    if (g_probe_tags[set] == line) {
      g_probe_hits++;
    } else {
      g_probe_misses++;
      g_probe_tags[set] = line;
    }

    // (b) distinct-line bitmap -- relative to the table base.
    if (g_probe_bitmap != nullptr && addr >= g_probe_base) {
      uint64_t rel = (addr - g_probe_base) >> 6;
      if (rel < g_probe_span_lines) {
        uint64_t byte = rel >> 3;
        uint8_t mask = (uint8_t)(1u << (rel & 7));
        if ((g_probe_bitmap[byte] & mask) == 0) {
          g_probe_bitmap[byte] |= mask;
          g_distinct_lines++;
        }
        return;
      }
    }
    g_probe_oob++;
  }
#endif
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
  unsigned long rc_deferred = (unsigned long)__atomic_load_n(&g_rc_deferred, __ATOMIC_RELAXED);
  unsigned long rc_applied = (unsigned long)__atomic_load_n(&g_rc_applied, __ATOMIC_RELAXED);
  unsigned long rc_coalesced = (unsigned long)__atomic_load_n(&g_rc_coalesced, __ATOMIC_RELAXED);
  unsigned long rc_overflow = (unsigned long)__atomic_load_n(&g_rc_overflow, __ATOMIC_RELAXED);
  unsigned long rc_self_flushes = (unsigned long)__atomic_load_n(&g_rc_self_flushes, __ATOMIC_RELAXED);
  unsigned long rc_log_hwm = (unsigned long)__atomic_load_n(&g_rc_log_hwm, __ATOMIC_RELAXED);

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

  // Deferred-RC tallies (Levanoni-Petrank). Zero in eager mode (ALASKA_DEFER_RC unset/0)
  // and in non-measurement builds. rc_deferred+rc_overflow == inc-barrier calls; incref ==
  // rc_applied + rc_overflow (the invariant the A/B checks across defer modes).
  fprintf(f,
      "rc_deferred=%lu\nrc_applied=%lu\nrc_coalesced=%lu\n"
      "rc_overflow=%lu\nrc_self_flushes=%lu\nrc_log_hwm=%lu\n",
      rc_deferred, rc_applied, rc_coalesced, rc_overflow, rc_self_flushes, rc_log_hwm);

#if ALASKA_ENABLE_CACHE_PROBE
  // Cache-miss characterization probe results (see EventCounters.cpp). probe_misses
  // / (probe_hits + probe_misses) is the modeled Mapping-line miss rate for a
  // direct-mapped cache of probe_sets lines; probe_distinct_lines is the working set.
  fprintf(f,
      "probe_distinct_lines=%lu\nprobe_total_touches=%lu\n"
      "probe_hits=%lu\nprobe_misses=%lu\nprobe_oob=%lu\nprobe_sets=%lu\n",
      (unsigned long)alaska::events::g_distinct_lines,
      (unsigned long)alaska::events::g_total_line_touches,
      (unsigned long)alaska::events::g_probe_hits,
      (unsigned long)alaska::events::g_probe_misses,
      (unsigned long)alaska::events::g_probe_oob,
      (unsigned long)alaska::events::g_probe_sets);
#endif

  if (to_file) {
    fclose(f);
  } else {
    fflush(f);
  }
}
