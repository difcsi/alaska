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

#include <alaska/alaska.hpp>
#include <alaska/core/Runtime.hpp>
#include <alaska/gc_bitmaps.hpp>
#include <alaska/EventCounters.hpp>
#include <ck/vec.h>
#include <stdint.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Reference counting + (opt-in) zero-refcount reclamation.
//
// PORTED FROM main-rc, adapted to dev:
//  * dev keeps its own Mapping layout (a fully-packed 8-byte word with no spare
//    bit), so the per-Mapping "on-nullcount" HINT bit main-rc used is dropped --
//    the zero-refcount set lives ENTIRELY in the side bitmap (gc_bitmaps'
//    nullcount_bm_*), which is the source of truth. The reclaim scan re-validates
//    every entry's refcount, so a stale bit can never cause a wrong free; the scan
//    self-cleans bits it finds resurrected (refcount != 0).
//  * The reclaim uses dev's existing stop-the-world barrier and its handle PINNING
//    (alaska::barrier pins every handle found on a thread stack) in place of
//    main-rc's separate "present" stackscan bitmap: a zero-refcount handle that is
//    pinned is on some stack, so it is kept for a later cycle.
//  * main-rc's deferred (Levanoni-Petrank) increments, the per-Mapping resurrection
//    hint, conservative dec-on-free (alaska_hfree_dec_children) and copy-inc
//    (alaska_inc_handles_in_range) are NOT ported here yet -- they depend on
//    main-rc-only primitives (Mapping::is_live_handle / could_be_aligned_handle,
//    heap.pt.get_unaligned, ALASKA_KEEP_HANDLE_ALIVE). See PORTING_PLAN.md. The
//    reclaim/nullcount machinery below is gated behind ALASKA_ENABLE_CYCLE_COLLECTION
//    (default OFF on dev), so the default build's refcount behavior is unchanged.
// ---------------------------------------------------------------------------

// The global hfree entry point (halloc.cpp): frees an object and its mapping.
extern "C" void hfree(void *ptr);
// Cycle-collector candidate hook (CycleCollector.cpp); stubbed when the gate is off.
extern "C" void alaska_cycle_register_candidate(void *ptr);

extern "C" {

// Thread-local guard to prevent recursion. Some refcount operations can, in
// principle, trigger further refcount operations while touching handle metadata;
// the guard makes those re-entrant calls no-ops.
static thread_local bool in_refcount_operation = false;

/**
 * alaska_inc_refcount - Increment the reference count of a handle.
 *
 * Called by the compiler's RefcountInc pass when a handle is written to memory.
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_inc_refcount(void *ptr) {
  if (ptr == nullptr) return;
  if (in_refcount_operation) return;

  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do.
    return;
  }

  in_refcount_operation = true;
  mapping->inc_refcount();
#if ALASKA_ENABLE_CYCLE_COLLECTION
  // Resurrected a (possibly) zero-refcount handle: drop it from the zero set. The
  // reclaim scan re-validates anyway, so this is only to bound the bitmap; without
  // the per-Mapping hint we simply clear unconditionally on inc.
  alaska::gc::nullcount_bm_clear(mapping);
#endif
  in_refcount_operation = false;
}

/**
 * alaska_dec_refcount - Decrement the reference count of a handle.
 *
 * Called by the compiler's RefcountDec pass when a handle reference is overwritten.
 * When the count reaches zero the handle is recorded in the nullcount side bitmap
 * for the (opt-in) reclaim pass to consider.
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_dec_refcount(void *ptr) {
  if (ptr == nullptr) return;
  if (in_refcount_operation) return;

  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do.
    return;
  }

  in_refcount_operation = true;
  uint64_t new_count = mapping->dec_refcount();
#if ALASKA_ENABLE_CYCLE_COLLECTION
  if (new_count == 0) {
    // Record as a zero-refcount handle for the reclaim pass.
    alaska::gc::nullcount_bm_set(mapping);
  } else {
    // Still non-zero: the only situation in which the handle can be the root of a
    // garbage cycle, so buffer it as a candidate for the cycle collector.
    alaska_cycle_register_candidate(ptr);
  }
#else
  (void)new_count;
#endif
  in_refcount_operation = false;
}

// Bridge from the cycle collector's reclaim() to the zero-refcount set (weak symbol
// it links against). A reclaimed cycle member is added to the nullcount set and
// freed by the next alaska_refcount_reclaim().
void alaska_nullcount_add(void *handle) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m) alaska::gc::nullcount_bm_set(m);
#else
  (void)handle;
#endif
}

/**
 * alaska_get_refcount - Get the current reference count of a handle.
 * Returns the refcount if ptr is a valid handle, else 0.
 */
unsigned long alaska_get_refcount(void *ptr) {
  if (ptr == nullptr) return 0;
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) return 0;
  return (unsigned long)mapping->get_refcount();
}

// --- Nullcount (zero-refcount set) query API --------------------------------
// These wrap the side bitmap and are defined unconditionally so the ported tests
// link regardless of the cycle-collection gate; when the gate is off nothing ever
// populates the bitmap, so they simply report an empty set.

int alaska_nullcount_map_size(void) {
  return (int)alaska::gc::nullcount_bm_count();
}

void alaska_nullcount_map_foreach(void (*fn)(void *ptr)) {
  ck::vec<alaska::Mapping *> cands;
  alaska::gc::nullcount_bm_collect(cands);
  for (auto *m : cands)
    fn(m->to_handle());
}

// Copy the current zero-refcount handle set into `out` (capacity `cap`); returns
// the total number of entries (grow and retry if it exceeds `cap`).
size_t alaska_nullcount_snapshot(void **out, size_t cap) {
  ck::vec<alaska::Mapping *> cands;
  alaska::gc::nullcount_bm_collect(cands);
  size_t n = 0;
  for (auto *m : cands)
    if (n < cap) out[n++] = m->to_handle();
  return cands.size();
}

// Drop a handle from the zero-refcount set. Called by hfree when freeing a handle
// that reached refcount 0, so a recycled slot never inherits a stale collectable bit.
void alaska_nullcount_forget(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m != nullptr) alaska::gc::nullcount_bm_clear(m);
}

/**
 * alaska_refcount_reclaim - Reclaim zero-refcount handles (opt-in).
 *
 * Runs a stop-the-world barrier, then scans the nullcount bitmap and frees every
 * handle that (a) is a valid, live handle, (b) still has refcount 0, and (c) is
 * NOT pinned (i.e. not found on any thread stack by the barrier's conservative
 * scan). Entries that fail (a)/(b) are self-cleaned from the bitmap; pinned ones
 * are kept for a later cycle. Returns the number of handles freed.
 *
 * PORT-NOTE: freeing goes through dev's global hfree(). Whether hfree is fully
 * safe to call from inside the barrier callback on dev needs build validation;
 * this whole path is gated (ALASKA_ENABLE_CYCLE_COLLECTION) and off by default.
 */
size_t alaska_refcount_reclaim(void) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  size_t freed = 0;
  rt->with_barrier([&]() {
    ck::vec<alaska::Mapping *> cands;
    alaska::gc::nullcount_bm_collect(cands);
    for (auto *m : cands) {
      if (!rt->handle_table.valid_handle(m)) {
        alaska::gc::nullcount_bm_clear(m);
        continue;
      }
      if (m->get_refcount() != 0) {
        alaska::gc::nullcount_bm_clear(m);  // resurrected; self-clean
        continue;
      }
      if (m->is_pinned()) continue;  // on some stack: keep for a later cycle
      alaska::gc::nullcount_bm_clear(m);
      hfree(m->to_handle());
      freed++;
    }
  });
#if ALASKA_ENABLE_EVENT_COUNTERS
  alaska::events::gc_free_event(freed);
#endif
  return freed;
#else
  return 0;
#endif
}

}  // extern "C"
