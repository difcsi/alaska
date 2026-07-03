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
#include <alaska/core/ThreadCache.hpp>
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

// True only for a memory word that decodes to a real, LIVE handle. The conservative
// heap scans below feed arbitrary words here, so the decoded mapping MUST be
// bounds-checked (is_live_handle: pure pointer arithmetic, no deref) before any
// field is touched -- from_handle structurally decodes ANY sign-bit-set value, which
// for junk data is a wild pointer that inc/dec_refcount would dereference and crash on.
static inline bool word_is_live_handle(void *p) {
  if (!alaska::Mapping::could_be_aligned_handle(p)) return false;
  auto *m = alaska::Mapping::from_handle(p);
  return alaska::Runtime::get().handle_table.is_live_handle(m);
}

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
#if ALASKA_ENABLE_DEFER_RC
  // Deferred (Levanoni-Petrank) mode: append the increment to this thread's log to be
  // applied in a batch at the barrier (ThreadCache::drain_inc), keeping the scattered
  // handle-table CAS off the store-barrier hot path. On overflow / no thread cache, fall
  // through to an eager increment so no increment is ever lost.
  {
    auto *tc = alaska::ThreadCache::current();
    if (likely(tc != nullptr && tc->defer_inc(mapping))) {
      in_refcount_operation = false;
      return;
    }
  }
#endif
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
 * alaska_hfree_dec_children - Drop the references an aggregate held, on free.
 *
 * When an object is freed its outgoing handle references die too, so decrement each
 * contained (validated live) handle's reference count. This is what finally lets
 * refcounts return to zero for handles only reachable through heap containers:
 * without it a stored-once handle is inc'd (on the heap write barrier) but never
 * dec'd, so its count never reaches 0 and reclamation never triggers. Paired with
 * copy-inc (alaska_inc_handles_in_range) so memcpy'd references stay balanced.
 *
 * Ported from main-rc. Defined always (so links regardless of gate); the work is
 * gated behind ALASKA_ENABLE_CYCLE_COLLECTION so dev's default hfree is unchanged.
 * CAVEAT (main-rc): conservative dec-on-free is UNSOUND for programs that manage
 * self-referential structures explicitly -- an undercounted reference can drive a
 * live node to 0 and have the GC reclaim it. Opt out at runtime with ALASKA_NO_FREE_DEC=1.
 * PORT-NOTE: only handles SIZED handles (dev huge-object-without-mapping path skipped).
 */
void alaska_hfree_dec_children(void *ptr) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_NO_FREE_DEC") != nullptr) ? 0 : 1;
  if (!enabled) return;
  if (ptr == nullptr || in_refcount_operation) return;

  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr || m->is_free()) return;  // only real, live SIZED handles
  void **words = (void **)m->get_pointer();
  if (words == nullptr) return;

  auto *tc = alaska::ThreadCache::current();
  if (tc == nullptr) return;
  size_t size = tc->get_size(ptr);
  if (size < sizeof(void *)) return;

  size_t n = size / sizeof(void *);
  for (size_t i = 0; i < n; i++) {
    if (words[i] != nullptr && word_is_live_handle(words[i])) alaska_dec_refcount(words[i]);
  }
  // Keep ptr's handle live across the scan so compaction cannot relocate the object
  // out from under `words` if a caller ever runs this without the world stopped.
  ALASKA_KEEP_HANDLE_ALIVE(ptr);
#else
  (void)ptr;
#endif
}

/**
 * alaska_inc_handles_in_range - Inc the handles a byte copy deposited.
 *
 * The store barrier (RefcountInc) only increments on pointer-TYPED stores, so a
 * memcpy/memmove of handle-containing memory creates new heap references WITHOUT
 * incrementing them -- they would then be dec'd on free with no matching inc, an
 * underflow that could free a live object. The compiler instruments every byte copy
 * with a call here over the destination range to keep inc/dec balanced.
 *
 * Ported from main-rc. Defined ALWAYS (the compiler emits calls to it); the work is
 * gated so it is a no-op in dev's default (non-reclaiming) build. Opt out at runtime
 * with ALASKA_NO_COPY_INC=1.
 */
void alaska_inc_handles_in_range(void *base, size_t bytes) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_NO_COPY_INC") != nullptr) ? 0 : 1;
  if (!enabled) return;
  if (base == nullptr || bytes < sizeof(void *)) return;
  if (in_refcount_operation) return;

  void **words = (void **)base;
  // Recover the destination object's handle once and keep it live across the scan so
  // a barrier that fires mid-scan (this runs lock-free on a mutator) cannot let
  // compaction relocate the copy destination and leave `words` dangling.
  void *dest_handle = nullptr;
  if (auto *tc = alaska::ThreadCache::current()) {
    if (auto *dm = tc->reverse_lookup(base)) dest_handle = dm->to_handle();
  }
  size_t n = bytes / sizeof(void *);
  for (size_t i = 0; i < n; i++) {
    if (words[i] != nullptr && word_is_live_handle(words[i])) alaska_inc_refcount(words[i]);
  }
  if (dest_handle) ALASKA_KEEP_HANDLE_ALIVE(dest_handle);
#else
  (void)base;
  (void)bytes;
#endif
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
#if ALASKA_ENABLE_DEFER_RC
    // Apply pending deferred increments before reading any refcount, so a handle with a
    // logged-but-unapplied inc is back at full count and not freed here (Levanoni-Petrank).
    for (auto *tcx : rt->tcs)
      tcx->drain_inc();
#endif
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
