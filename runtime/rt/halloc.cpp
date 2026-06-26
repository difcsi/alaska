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

// #define MALLOC_BYPASS

#ifdef MALLOC_BYPASS
#include <malloc.h>
#endif

#include <ck/stack.h>
#include <ck/queue.h>
#include <ck/vec.h>
#include <ck/set.h>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/EventCounters.hpp>
#include <alaska.h>
#include <errno.h>
#include <stdlib.h>



// TODO: don't have this be global!
static __thread alaska::ThreadCache *g_tc = nullptr;

// liballocs integration (weak: builds/runs without liballocs). On each
// halloc/hfree we hand liballocs the object's *backing base* so it can bind the
// pending allocation site (stashed by allocscc in liballocs' __current_allocsite)
// to that object, enabling typed metadata queries. See
// contrib/liballocs/src/allocators/alaska.c.
extern "C" void __liballocs_notify_alaska_alloc(void *backing_base, unsigned long requested_size)
    __attribute__((weak));
extern "C" void __liballocs_notify_alaska_free(void *backing_base) __attribute__((weak));

#if ALASKA_ENABLE_REFCOUNT
// Defined in refcount.cpp: on free, decrement the reference counts of the handles
// the object held (see alaska_hfree_now).
extern "C" void alaska_hfree_dec_children(void *ptr);
#endif

// The backing base liballocs should bind metadata to. For a sized object that is
// the mapping's backing pointer; for a HUGE object there is no mapping -- the value
// halloc returned IS the raw backing pointer, so hand that over directly. Returns
// null only for a freed/invalid sized handle. (Huge raw pointers are positive, so
// from_handle_safe returns null for them -- that is exactly the huge case.)
static inline void *alaska_liballocs_backing_base(void *result) {
  auto *m = alaska::Mapping::from_handle_safe(result);
  if (m == nullptr) return result;            // huge object: result is the backing base
  return m->is_free() ? nullptr : m->get_pointer();
}

// Base-level notifications: bind/drop liballocs metadata for an already-resolved
// backing base (used by the realloc path, which must capture the OLD base before
// the object moves).
static inline void alaska_liballocs_notify_alloc_base(void *base, size_t requested_size) {
  if (__liballocs_notify_alaska_alloc != nullptr && base != nullptr)
    __liballocs_notify_alaska_alloc(base, requested_size);
}

static inline void alaska_liballocs_notify_free_base(void *base) {
  if (__liballocs_notify_alaska_free != nullptr && base != nullptr)
    __liballocs_notify_alaska_free(base);
}

static inline void alaska_liballocs_notify_alloc(void *handle, size_t requested_size) {
  if (__liballocs_notify_alaska_alloc == nullptr) return;  // liballocs not loaded
  alaska_liballocs_notify_alloc_base(alaska_liballocs_backing_base(handle), requested_size);
}

static inline void alaska_liballocs_notify_free(void *handle) {
  if (__liballocs_notify_alaska_free == nullptr) return;  // liballocs not loaded
  alaska_liballocs_notify_free_base(alaska_liballocs_backing_base(handle));
}



alaska::ThreadCache *get_tc_r(void) {
  if (unlikely(g_tc == nullptr)) {
    g_tc = alaska::Runtime::get().new_threadcache();
  }
  return g_tc;
}
alaska::LockedThreadCache get_tc(void) { return *get_tc_r(); }




static void *_halloc(size_t sz, int zero) {
  void *result = get_tc()->halloc(sz, zero);

  // This seems right...
  if (result == NULL) errno = ENOMEM;
  else {
    alaska_liballocs_notify_alloc(result, sz);  // bind allocsite + size (halloc + hcalloc)
#if ALASKA_ENABLE_EVENT_COUNTERS
    alaska::events::halloc_event();
#endif
  }
  return result;
}


void *halloc(size_t sz) noexcept {
#ifdef MALLOC_BYPASS
  return ::malloc(sz);
#endif
  // Zero handle allocations. The refcount dec write-barrier reads a slot's old
  // value before overwriting it (load-before-store) and hands it to
  // alaska_dec_refcount; a first store into freshly-allocated-but-uninitialized
  // heap would otherwise dec whatever garbage was there, which the runtime can
  // decode as a bogus handle and fault on. Zeroing guarantees an as-yet-unwritten
  // slot reads as null, which dec ignores. (hcalloc already zeroes.)
  return _halloc(sz, 1);
}

void *hcalloc(size_t nmemb, size_t size) {
#ifdef MALLOC_BYPASS
  return ::calloc(nmemb, size);
#endif
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
void *hrealloc(void *handle, size_t new_size) {
#ifdef MALLOC_BYPASS
  return ::realloc(handle, new_size);
#endif
  // If the handle is null, then this call is equivalent to malloc(size)
  if (handle == NULL) return halloc(new_size);


  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m == NULL) {
    if (!alaska::Runtime::get().heap.huge_allocator.owns(handle)) {
      log_debug("realloc edge case: not a handle %p!", handle);
      return ::realloc(handle, new_size);
    }
  }

  // If the size is equal to zero, and the handle is not null, realloc acts like free(handle)
  if (new_size == 0) {
    log_debug("realloc edge case: zero size %p!", handle);
    // Unconditional backing free (bypass any liballocs hfree interposition).
    alaska_hfree_now(handle);
    return NULL;
  }

  // Capture the old backing base BEFORE the realloc moves/frees the object, so we
  // can drop its liballocs metadata afterward.
  bool old_was_handle = alaska::Mapping::is_handle(handle);
  void *old_base = alaska_liballocs_backing_base(handle);

  handle = get_tc()->hrealloc(handle, new_size);

  // Seed liballocs metadata for any realloc that touches a HUGE object on either
  // side. ThreadCache::hrealloc only relocates/seeds the trailing insert for the
  // handle->handle case (where it memcpy's the old insert to its new offset); the
  // huge-producing and huge-consuming cases come back from huge_allocator.allocate
  // / allocate_backing_data with the reserve in place but the insert uninitialised
  // and no side-table record. Seed them exactly like _halloc would (GC bit + exact
  // size + allocsite), and forget the old object's now-stale record. The pure
  // handle->handle case is left untouched so its preserved lifetime-policy mask
  // (e.g. a manual pin) survives the move.
  bool new_is_handle = handle != NULL && alaska::Mapping::is_handle(handle);
  if (handle != NULL && !(old_was_handle && new_is_handle)) {
    void *new_base = alaska_liballocs_backing_base(handle);
    if (old_base != nullptr && old_base != new_base) alaska_liballocs_notify_free_base(old_base);
    alaska_liballocs_notify_alloc_base(new_base, new_size);
  }
  return handle;
}



// The genuine backing free. liballocs does NOT interpose this symbol, so the
// lifetime-policy machinery (and internal runtime callers like hrealloc and the
// in-barrier reclaim's passthrough) use it to actually reclaim a handle.
void alaska_hfree_now(void *ptr) {
#ifdef MALLOC_BYPASS
  return ::free(ptr);
#endif
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;

#if ALASKA_ENABLE_EVENT_COUNTERS
  // Program-initiated free. The GC reclaim path frees via tc.hfree directly (not
  // through here), so it is tallied separately by gc_free_event and not counted here.
  alaska::events::hfree_event();
#endif

#if ALASKA_ENABLE_REFCOUNT
  // Deferred dec-on-free is the DEFAULT: move the expensive alaska_hfree_dec_children scan
  // + backing free off this hot path onto the barrier thread, which drains the per-thread
  // queue with the world stopped (see ThreadCache::defer_free / drain_deferred and
  // barrier_thread_func). Opt out with ALASKA_NO_FREE_DEC_DEFER=1 (run the scan inline
  // instead) or disable dec-on-free entirely with ALASKA_NO_FREE_DEC=1 (then the inline
  // call below is itself a no-op). Still drop liballocs metadata NOW, while the mapping
  // resolves; the deferred backing free does not re-notify (tc.hfree never calls
  // liballocs). The enqueue runs under tc.lock (the get_tc() LockedThreadCache).
  static int defer = -1;
  if (defer < 0) {
    bool free_dec_on = (getenv("ALASKA_NO_FREE_DEC") == nullptr);
    bool defer_off = (getenv("ALASKA_NO_FREE_DEC_DEFER") != nullptr);
    defer = (free_dec_on && !defer_off) ? 1 : 0;
  }
  if (defer) {
    alaska_liballocs_notify_free(ptr);
    get_tc()->defer_free(ptr);
    return;
  }

  // Inline path (taken only when deferral is opted out): drop the references this aggregate
  // held before its memory is recycled, so a container's outgoing handles can reach refcount
  // 0 and be reclaimed. Must run while the object is still readable -- i.e. before the
  // backing free below. A no-op if ALASKA_NO_FREE_DEC=1.
  //
  // H4: if deferred reference counting (ALASKA_DEFER_RC) is also on, apply THIS thread's own
  // pending increments before the dec-on-free scan, else a deferred inc to one of these
  // children could be undercounted into a premature free. Drain via get_tc() so it runs under
  // tc.lock (a concurrent barrier cannot then drain the same log); a no-op when deferral is off.
  get_tc()->drain_inc();
  alaska_hfree_dec_children(ptr);
#endif

  // Drop any liballocs metadata binding before the backing memory is recycled.
  alaska_liballocs_notify_free(ptr);

#ifdef ALASKA_HTLB_SIM
  extern void alaska_htlb_sim_invalidate(uintptr_t handle);
  alaska_htlb_sim_invalidate((uintptr_t)ptr);
#endif

  // Simply ask the thread cache to free it!
  get_tc()->hfree(ptr);
}

// Public manual-free entry point. When liballocs is loaded it interposes this
// symbol and turns the call into "detach the liballocs manual lifetime policy"
// (interop; see contrib/liballocs/src/allocators/alaska.c); the native GC then
// finalizes the backing. Without an interposer this is just an unconditional free.
void hfree(void *ptr) { alaska_hfree_now(ptr); }




size_t alaska_usable_size(void *ptr) {
#ifdef MALLOC_BYPASS
  return ::malloc_usable_size(ptr);
#endif
  return get_tc()->get_size(ptr);
}




template <typename Fn>
static void walk_structure(void *ptr, size_t max_depth, Fn fn) {
  auto &rt = alaska::Runtime::get();
  auto *tc = get_tc_r();
  ck::queue<void *> todo(max_depth);

  auto schedule_pointer = [&](void *h, alaska::Mapping *m) {
    // is_live_handle, not valid_handle + !is_free: a slot on the allocator free list keeps
    // its invl bit clear, so is_free() would pass it and we'd walk a raw next-link as if it
    // were object data. See HandleTable::is_live_handle.
    if (not rt.handle_table.is_live_handle(m)) return;
    fn(m);
    if (todo.size() >= max_depth) return;
    todo.push(h);
  };

  auto *m = alaska::Mapping::from_handle_safe(ptr);
  // Fire off a check of the first pointer to bootstrap the localization
  schedule_pointer(ptr, m);

  while (not todo.is_empty()) {
    auto *h = todo.pop().unwrap();
    auto *m = alaska::Mapping::from_handle_safe(h);
    if (m == nullptr) continue;
    long size = tc->get_size(h);
    long elements = size / 8;

    void **cursor = (void **)m->get_pointer();
    for (long e = 0; e < elements; e++) {
      void *c = cursor[e];
      auto *m = alaska::Mapping::from_handle_safe(c);
      if (m) {
        schedule_pointer(c, m);
      }
    }
    // `cursor` is h's raw backing, held across this inner scan; keep h pinned so a barrier mid-walk
    // cannot let compaction relocate the object and dangle `cursor`. See ALASKA_KEEP_HANDLE_ALIVE.
    ALASKA_KEEP_HANDLE_ALIVE(h);
  }
}

static size_t get_page_count_for_structure(void *ptr, size_t max_depth) {
  max_depth = 256;
  ck::set<off_t> pages;
  walk_structure(ptr, max_depth, [&](alaska::Mapping *m) {
    pages.add((off_t)m->get_pointer() >> 12);
  });

  return pages.size();
}


extern "C" bool localize_structure(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return false;
  auto *tc = get_tc_r();
  auto &rt = alaska::Runtime::get();

  size_t max_depth = 26;

  auto pages_before = get_page_count_for_structure(ptr, max_depth * 2);

  // Trigger a barrier so we can move stuff
  bool localized = rt.with_barrier([&]() {
    walk_structure(ptr, max_depth, [&](alaska::Mapping *m) {
      if (not m->is_pinned()) {
        tc->localize(*m, rt.localization_epoch);
      }
    });
  });

  auto pages_after = get_page_count_for_structure(ptr, max_depth * 2);

  if (localized)
    printf("pages: %zu, %zu (%f%%)\n", pages_before, pages_after,
        pages_after / (float)pages_before * 100.0);
  return localized;
}
