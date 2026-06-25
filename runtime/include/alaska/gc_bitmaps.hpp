/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Stackscan "present" side bitmap.
 *
 * The collector needs one bit of per-handle state that does not fit in the
 * fully-packed 8-byte alaska::Mapping (static_assert sizeof==8, no spare bits):
 *
 *   present  -- "this handle is currently on some thread's stack/registers",
 *               set by a conservative scan during a stop-the-world barrier.
 *
 * It is a flat bitmap, one bit per handle-table slot, indexed by the mapping's
 * linear offset from the (fixed) handle-table base. `present_mark` is an atomic
 * OR into preallocated storage and is therefore async-signal-safe -- it is called
 * from the barrier signal handler on each participating thread. `present_clear`
 * reallocates/zeroes and is only called on the barrier thread before the barrier.
 * `present_test` is read on the barrier thread inside the barrier callback.
 */

#pragma once

#include <alaska/alaska.hpp>
#include <ck/vec.h>

namespace alaska::gc {

  // Reset + size to the current handle-table capacity. Barrier thread, pre-barrier.
  void present_clear(void);
  // Mark a handle present. Async-signal-safe (atomic OR, no allocation).
  void present_mark(alaska::Mapping *m);
  // Test whether a handle was marked present this cycle. Barrier callback.
  bool present_test(alaska::Mapping *m);

  // --- Nullcount side bitmap ---------------------------------------------------
  // One bit per handle-table slot marking "this handle's refcount is currently 0".
  // This is the zero-refcount set that refcount.cpp's reclaim consumes: set on
  // dec->0, cleared on inc->1 / free, scanned by reclaim. Because the handle-table
  // base is fixed (HandleTable::grow remaps in place) the bitmap is allocated ONCE
  // over a generous NORESERVE reservation, so set/clear are a single lock-free
  // atomic OR/AND with no rehash and no resize -- a mutator parked mid-update can
  // never leave it inconsistent, so the in-barrier scan needs no lock and never
  // skips a cycle. (It replaced a lock-guarded hashmap; see refcount.cpp.)
  void nullcount_bm_set(alaska::Mapping *m);    // dec->0: mark zero-refcount
  void nullcount_bm_clear(alaska::Mapping *m);  // inc->1 / free: unmark
  // Append every currently-set handle to `out`. World-stopped (barrier) scan.
  void nullcount_bm_collect(ck::vec<alaska::Mapping *> &out);
  // popcount of the whole bitmap (diagnostics / map_size parity).
  size_t nullcount_bm_count(void);

}  // namespace alaska::gc
