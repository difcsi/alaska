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

#include <alaska/ThreadCache.hpp>
#include <alaska/Logger.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/alaska.hpp>
#include "alaska/Heap.hpp"
#include "alaska/HeapPage.hpp"
#include <alaska/utils.h>

// Drop a freed handle from the GC's zero-refcount nullcount set (defined in
// rt/refcount.cpp, which is only linked into libalaska -- the reverse of the
// core->rt dependency -- so it is weak and the call is address-guarded, mirroring
// CycleCollector.cpp's use of alaska_nullcount_add). Lets hfree keep the nullcount
// set in step with slot reuse.
extern "C" void alaska_nullcount_forget(void *handle) __attribute__((weak));

#if ALASKA_ENABLE_REFCOUNT
// Drop the references an object held, on free (decrement each contained handle's refcount).
// Defined in rt/refcount.cpp -- weak and address-guarded for the same core->rt link-direction
// reason as alaska_nullcount_forget above. Internally a no-op if ALASKA_NO_FREE_DEC is set.
// Used by the deferred dec-on-free path (defer_free / drain_deferred).
extern "C" void alaska_hfree_dec_children(void *handle) __attribute__((weak));
#endif

// Number of bytes to reserve at the END of every sized backing allocation for a
// liballocs trailing `struct insert` (which carries the per-object lifetime-policy
// mask). Injected by the stackscan build via -DALASKA_LIBALLOCS_INSERT_RESERVE=8;
// defaults to 0 (vanilla Alaska -- no reservation, no behavior change). The reserve
// is folded into the object's size_of() so GC compaction copies the insert with the
// object, while liballocs keeps reporting the caller's exact requested size via its
// side-table record. See docs/liballocs-alaska-integration.md.
#ifndef ALASKA_LIBALLOCS_INSERT_RESERVE
#define ALASKA_LIBALLOCS_INSERT_RESERVE 0
#endif


namespace alaska {



  ThreadCache::ThreadCache(int id, alaska::Runtime &rt)
      : id(id)
      , runtime(rt)
      , localizer(rt.config, *this) {
    handle_slab = runtime.handle_table.new_slab(this);
  }


  void *ThreadCache::allocate_backing_data(const alaska::Mapping &m, size_t size) {
    // Reserve room at the end of the slot for liballocs' trailing insert (0 unless
    // the build injected a reserve). Sizing AND the page's recorded slack use the
    // padded size, so size_of() == size + reserve and the reserved tail is copied
    // intact when the GC relocates the object.
    size_t slot = size + ALASKA_LIBALLOCS_INSERT_RESERVE;
    int cls = alaska::size_to_class(slot);
    SizedPage *page = size_classes[cls];
    if (unlikely(page == nullptr)) page = new_sized_page(cls);
    void *ptr = page->alloc(m, slot);
    if (unlikely(ptr == nullptr)) {
      // OOM?
      page = new_sized_page(cls);
      ptr = page->alloc(m, slot);
      ALASKA_ASSERT(ptr != nullptr, "OOM!");
    }
    return ptr;
  }




  void ThreadCache::free_allocation(const alaska::Mapping &m) {
    void *ptr = m.get_pointer();
    // Grab the page from the global heap (walk the page table).
    auto *page = this->runtime.heap.pt.get_unaligned(ptr);
    if (unlikely(page == NULL)) {
      this->runtime.heap.huge_allocator.free(ptr);
      return;
    }
    ALASKA_ASSERT(page != NULL, "calling hfree should always return a heap page");

    if (page->is_owned_by(this)) {
      log_trace("Free handle %p locally (ptr = %p)", &m, ptr);
      page->release_local(m, ptr);
    } else {
      log_trace("Free handle %p remotely (ptr = %p)", &m, ptr);
      page->release_remote(m, ptr);
    }
  }




#if ALASKA_ENABLE_REFCOUNT
  void ThreadCache::flush_reuse_class(int cls) {
    ReuseEntry &e = reuse_cache[cls];
    if (e.m == nullptr) return;
    alaska::Mapping *m = e.m;
    e = ReuseEntry{};
    // Reclaim exactly as the normal hfree_impl tail does: free the backing wherever it
    // now lives (free_allocation re-walks the page table, so a swapped-out page is freed
    // local-or-remote correctly), clear the pointer, and return the slot to the table.
    free_allocation(*m);
    m->set_pointer(nullptr);
    this->runtime.handle_table.put(m, this);
  }

  void ThreadCache::flush_reuse_cache(void) {
    for (int cls = 0; cls < alaska::num_size_classes; cls++)
      flush_reuse_class(cls);
  }
#endif


  SizedPage *ThreadCache::new_sized_page(int cls) {
#if ALASKA_ENABLE_REFCOUNT
    // The cached entry for this class lives on the page we are about to swap out; reclaim it
    // the normal way before it stops being our current page.
    flush_reuse_class(cls);
#endif
    // Get a new heap
    auto *heap = runtime.heap.get_sizedpage(alaska::class_to_size(cls), this);

    // And set the owner
    heap->set_owner(this);

    // Swap the heaps in the thread cache
    if (size_classes[cls] != nullptr) runtime.heap.put_page(size_classes[cls]);
    size_classes[cls] = heap;

    ALASKA_ASSERT(heap->available() > 0, "New heap must have space");
    return heap;
  }


  LocalityPage *ThreadCache::new_locality_page(size_t required_size) {
    // Get a new heap
    auto *lp = runtime.heap.get_localitypage(required_size, this);

    // Swap the heaps in the thread cache
    if (this->locality_page != nullptr) runtime.heap.put_page(this->locality_page);
    this->locality_page = lp;

    ALASKA_ASSERT(lp->available() > 0, "New heap must have space");
    return lp;
  }

  // Stub out the methods of ThreadCache
  void *ThreadCache::halloc(size_t size, bool zero) {

    if (unlikely(size == 0)) return NULL;

    if (unlikely(alaska::should_be_huge_object(size))) {
      log_debug("ThreadCache::halloc huge size=%zu\n", size);
      // Allocate the huge allocation.
      return this->runtime.heap.huge_allocator.allocate(size);
    }

    log_info("ThreadCache::halloc size=%zu", size);

#if ALASKA_ENABLE_REFCOUNT
    // Perceus-flavored fast path: reuse the slot+backing a uniquely-owned free stashed for
    // this size class. Classify exactly as allocate_backing_data (padded by the reserve) so
    // the class matches what the stash computed via size_of().
    {
      size_t slot = size + ALASKA_LIBALLOCS_INSERT_RESERVE;
      int cls = alaska::size_to_class(slot);
      ReuseEntry &e = reuse_cache[cls];
      if (e.m != nullptr) {
        if (e.page == size_classes[cls]) {
          Mapping *m = e.m;
          void *ptr = e.ptr;
          SizedPage *pg = e.page;
          e = ReuseEntry{};  // consume
          m->reset();        // refcount -> 0, flags cleared (matches a fresh new_mapping)
          m->set_pointer(ptr);
          pg->update_size_slack(ptr, slot);  // size_of() must reflect THIS request
          if (zero) memset(ptr, 0, size);    // MANDATORY: the cached backing is stale
          return m->to_handle();
        }
        // Stale entry (page swapped out): free it normally, then fall through to a fresh alloc.
        flush_reuse_class(cls);
      }
    }
#endif

    // Allocate a new mapping
    Mapping *m = new_mapping();
    log_info("ThreadCache::halloc mapping=%p", m);

#if ALASKA_ENABLE_REFCOUNT
    // Initialize the mapping with zero refcount for compiler's refcount tracking
    m->reset();
#endif

    void *ptr = allocate_backing_data(*m, size);
    if (zero) {
      memset(ptr, 0, size);
    }

    m->set_pointer(ptr);
    return m->to_handle();
  }


  void *ThreadCache::hrealloc(void *handle, size_t new_size) {
    // TODO: There is a race here... I think its okay, as a realloc really should
    // be treated like a UAF, and ideally another thread would not access the handle
    // while it is being reallocated.


    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    void *original_data = NULL;
    size_t original_size = 0;

    bool old_was_handle = m != nullptr;
    bool new_data_is_huge = alaska::should_be_huge_object(new_size);
    void *new_data = NULL;
    void *return_value = handle;


    // There are two case here:
    if (not old_was_handle) {
      // 1. The original object is a huge object, in which case the object is *not* a pointer, and
      //    we need to return a *different* value.
      original_data = handle;
      original_size = this->runtime.heap.huge_allocator.size_of(handle);
    } else {
      // 2. The original object is a handle, in which case we update the handle to point to the new
      //    data. This is the normal case.
      original_data = m->get_pointer();
      auto *page = this->runtime.heap.pt.get_unaligned(original_data);
      original_size = page->size_of(original_data);
    }

    // We should copy the minimum of the two sizes between the allocations.
    size_t copy_size = original_size > new_size ? new_size : original_size;


    // So now we have the original data and size, we need to make a new allocation and copy things
    // across. This has four major cases:

    if (old_was_handle and new_data_is_huge) {
      // 1. handle -> huge - we need to free the original handle and return a new huge object
      new_data = this->runtime.heap.huge_allocator.allocate(new_size);  // Allocate
      memcpy(new_data, original_data, copy_size);                       // Copy
      hfree(handle);                       // ThreadCache::hfree (member) -- not interposed
      return_value = new_data;
    } else if (not old_was_handle and new_data_is_huge) {
      // 2. huge -> huge - we need to free the original huge object and return a new huge object
      new_data = this->runtime.heap.huge_allocator.allocate(new_size);  // Allocate
      memcpy(new_data, original_data, copy_size);                       // Copy
      return_value = new_data;
    } else if (old_was_handle and not new_data_is_huge) {
      // 3. handle -> handle - we need to copy the data and update the handle
      new_data = this->allocate_backing_data(*m, new_size);  // Allocate
      memcpy(new_data, original_data, copy_size);            // Copy
      // Relocate liballocs' trailing insert (the per-object lifetime-policy mask)
      // to its NEW trailing position. The size changed, so the front copy above
      // leaves the insert at the OLD offset and the new trailing slot
      // uninitialized -- which loses the lifetime policy, so a still-referenced
      // object (e.g. one a pycallocs proxy keeps alive) could be reclaimed by the
      // GC after the move. original_size == size_of() includes the reserve, so the
      // old insert is at [original_size - reserve, original_size); the new one
      // belongs at [new_size, new_size + reserve).
      if (ALASKA_LIBALLOCS_INSERT_RESERVE) {
        memcpy((char *) new_data + new_size,
               (char *) original_data + (original_size - ALASKA_LIBALLOCS_INSERT_RESERVE),
               ALASKA_LIBALLOCS_INSERT_RESERVE);
      }
      free_allocation(*m);                                   // Free the original allocation
      m->set_pointer(new_data);                              // Update the handle
      return_value = handle;
    } else if (not old_was_handle and not new_data_is_huge) {
      // 4. huge -> handle - allocate a new handle, copy the data, and free the original huge object
      Mapping *m = new_mapping();                             // Allocate a new handle
      new_data = this->allocate_backing_data(*m, new_size);   // Allocate
      memcpy(new_data, original_data, copy_size);             // Copy
      m->set_pointer(new_data);                               // Update the handle
      this->runtime.heap.huge_allocator.free(original_data);  // Free the original huge object
      return_value = m->to_handle();
    }

    return return_value;
  }


  void ThreadCache::hfree(void *handle) { this->hfree_impl(handle, /*quarantine_slot=*/false); }

  void ThreadCache::hfree_impl(void *handle, bool quarantine_slot) {
    (void)quarantine_slot;  // unused unless ALASKA_ENABLE_REFCOUNT (deferred-drain path)
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (unlikely(m == nullptr)) {
      bool worked = this->runtime.heap.huge_allocator.free(handle);
      (void)worked;
      // ALASKA_ASSERT(worked, "huge free failed");
      return;
    }

#if ALASKA_ENABLE_REFCOUNT
    if(unlikely(m->get_refcount() > 1 )){
      alaska::printf("Warning: Freeing handle %p with a > 1 refcount %lu\n", handle, m->get_refcount());
    }
#endif
#if ALASKA_ENABLE_CYCLE_COLLECTION
    // Drop this handle from the cycle collector before its slot can be recycled,
    // so a later collection never traces a stale/reused mapping.
    this->runtime.cycle_collector.forget(m);
    // Likewise drop it from the zero-refcount nullcount set: if this handle reached
    // refcount 0 (e.g. its last heap reference was overwritten) it is listed as
    // collectable garbage, and leaving it there lets a future allocation that reuses
    // this mapping slot inherit a stale "on nullcount" entry and be wrongly reclaimed.
    // Gated on the per-Mapping hint so we only take the nullcount lock when needed,
    // and address-guarded since the symbol is weak (see the declaration above).
    if (m->is_on_nullcount() && &alaska_nullcount_forget) alaska_nullcount_forget(handle);
#endif

#if ALASKA_ENABLE_REFCOUNT
    // Perceus-flavored fast path: a uniquely-owned (refcount <= 1) handle freed on the
    // SYNCHRONOUS path is the only live reference, so its slot+backing can be re-handed
    // to the next same-size allocation without aliasing any survivor. Never on the
    // deferred-drain path (quarantine_slot) -- the quarantine forbids immediate reuse
    // there (the binarytrees UAF). Stash the still-live (mapping, backing) pair instead
    // of freeing it: the page Header keeps pointing at `m`, so reuse is near-free.
    // Also decline if the handle is on the zero-refcount nullcount set: leaving such a
    // slot live (stashed) would let the barrier's reclaim_dead_handles free its backing
    // underneath us (double free). The nullcount-forget above only runs under cycle
    // collection, so guard here independently (is_on_nullcount is available under refcount).
    if (!quarantine_slot && !m->is_pinned() && !m->is_free() && !m->is_on_nullcount() &&
        m->get_refcount() <= 1) {
      void *ptr = m->get_pointer();
      auto *page = this->runtime.heap.pt.get_unaligned(ptr);
      if (page != nullptr) {
        // size_of() is valid for any HeapPage; for a SizedPage it returns the padded slot
        // size, so size_to_class() recovers the page's own class. The `== size_classes[cls]`
        // check then guarantees: a SizedPage (LocalityPages are never in size_classes),
        // locally owned, and still this tc's current page for the class (so the alloc path
        // will hit it). Only stash when the per-class slot is empty -- otherwise fall
        // through to the normal free (no eviction; keeps the cache single-entry).
        int cls = alaska::size_to_class(page->size_of(ptr));
        if (cls >= 0 && cls < alaska::num_size_classes &&
            (alaska::HeapPage *)size_classes[cls] == page && reuse_cache[cls].m == nullptr) {
          // Leave the refcount as-is; it is zeroed at reuse (in halloc) so any spurious
          // inc/dec a conservative scanner makes during the stash window is wiped.
          reuse_cache[cls] = ReuseEntry{m, ptr, size_classes[cls]};
          return;
        }
      }
    }
#endif

    // Free the allocation behind a mapping
    free_allocation(*m);
    m->set_pointer(nullptr);
#if ALASKA_ENABLE_REFCOUNT
    if (quarantine_slot) {
      // Deferred-drain path: the backing is freed and the pointer cleared (so the slot's
      // backing is now 0 and HandleTable::is_live_handle already rejects it), but DON'T
      // return the slot to the allocatable pool yet. A parent still queued in some thread's
      // deferred_frees may hold this handle's value in a child word; recycling the slot now
      // lets the next allocation reuse it, after which that stale word resolves to a live
      // unrelated mapping and dec-on-free corrupts it (the binarytrees UAF). Hold the slot
      // for one barrier epoch -- quarantine_rotate releases it once every same-burst parent
      // has drained and harmlessly skipped this null-backing slot.
      quarantine_cur.push(m);
      return;
    }
#endif
    // Return the handle to the handle table.
    this->runtime.handle_table.put(m, this);
  }


#if ALASKA_ENABLE_REFCOUNT
  // Backpressure caps for deferred dec-on-free (see defer_free). Tunable; they bound how
  // long freed memory and handle slots sit undrained between barriers.
  static constexpr size_t kDeferredMaxCount = 8192;
  static constexpr size_t kDeferredMaxBytes = 8 * 1024 * 1024;  // 8 MiB queued backing
  static constexpr size_t kDeferredHugeBytes = 256 * 1024;      // >= this: free inline

  // Enqueue a program-freed handle for deferred dec-on-free instead of paying the
  // alaska_hfree_dec_children scan + backing free on the freeing thread's hot path. The
  // barrier thread drains the queue (drain_deferred) with the world stopped. CALLER MUST
  // HOLD this->lock (the get_tc() LockedThreadCache provides it on the hfree path).
  //
  // We replicate, NOW at enqueue, the GC-set bookkeeping ThreadCache::hfree does eagerly
  // (cycle_collector.forget + nullcount_forget). Otherwise a queued handle whose own
  // refcount is already 0 lingers in the nullcount set and the barrier's reclaim_dead_handles
  // frees it while it still sits in our queue -> double free.
  void ThreadCache::defer_free(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);

    // Huge objects have no mapping and hold a lot of memory: never queue them, free inline.
    // alaska_hfree_dec_children handles the huge case (ptr is the raw backing base), so the
    // huge object's handle-typed fields are still dec'd before its backing is freed -- the
    // scan just runs on the freeing thread, not the barrier (huge objects are too big to sit
    // in the queue).
    if (unlikely(m == nullptr)) {
      if (&alaska_hfree_dec_children) alaska_hfree_dec_children(handle);
      this->hfree(handle);
      return;
    }
    // Lenient skip for an already-freed handle. (Double free of a not-yet-drained handle is
    // UB, as for malloc; this only catches the easy is_free() case.)
    if (unlikely(m->is_free())) return;

    // Take the handle out of the GC's view immediately (mirrors ThreadCache::hfree).
#if ALASKA_ENABLE_CYCLE_COLLECTION
    this->runtime.cycle_collector.forget(m);
    if (m->is_on_nullcount() && &alaska_nullcount_forget) alaska_nullcount_forget(handle);
#endif

    // Large (but non-huge) objects also bypass the queue, to bound resident memory.
    size_t size = this->get_size(handle);
    if (size >= kDeferredHugeBytes) {
      if (&alaska_hfree_dec_children) alaska_hfree_dec_children(handle);
      this->hfree(handle);
      return;
    }

    deferred_frees.push(handle);
    deferred_bytes += size;

    // Backpressure: on exceeding either cap, drain this thread's whole queue inline now.
    // That is just today's behaviour batched -- safe because we hold this->lock and every
    // entry was freed by this thread.
    if ((size_t)deferred_frees.size() >= kDeferredMaxCount || deferred_bytes >= kDeferredMaxBytes) {
      this->drain_deferred();
    }
  }

  // Drain every queued handle: decrement its children then free its backing. The mapping
  // SLOT is NOT recycled here -- hfree_impl(quarantine_slot=true) parks it in the quarantine
  // so a still-queued parent's stale child word cannot alias a reused slot (see hfree_impl
  // and quarantine_rotate). Re-checks is_free() so a handle already reclaimed elsewhere is
  // skipped (keeps the drain idempotent w.r.t. reclaim_dead_handles). CALLER MUST HOLD
  // this->lock -- the barrier holds all tc locks via lock_all_thread_caches(); the inline
  // overflow path above already holds it.
  void ThreadCache::drain_deferred(void) {
    for (auto *h : deferred_frees) {
      alaska::Mapping *m = alaska::Mapping::from_handle_safe(h);
      if (m != nullptr && m->is_free()) continue;  // already reclaimed; drop
      if (&alaska_hfree_dec_children) alaska_hfree_dec_children(h);
      this->hfree_impl(h, /*quarantine_slot=*/true);
    }
    deferred_frees.clear_with_capacity();
    deferred_bytes = 0;
  }

  // Roll the deferred-drain handle-slot quarantine forward by one barrier epoch. Releases
  // the slots withheld a full epoch ago (quarantine_prev) back to the allocatable pool, then
  // promotes this epoch's withheld slots (quarantine_cur) into quarantine_prev. MUST run once
  // per barrier AFTER every thread's deferred queue has drained (see barrier_thread_func):
  // draining first guarantees any parent whose stale child word references a slot still in
  // quarantine_prev has already run its dec-on-free and skipped that null-backing slot, so
  // the slot is safe to recycle now. Two generations cover a free-burst that straddles a
  // barrier. CALLER MUST HOLD this->lock (the barrier holds all tc locks).
  void ThreadCache::quarantine_rotate(void) {
    for (auto *m : quarantine_prev)
      this->runtime.handle_table.put(m, this);
    quarantine_prev.clear_with_capacity();
    for (auto *m : quarantine_cur)
      quarantine_prev.push(m);
    quarantine_cur.clear_with_capacity();
  }
#endif  // ALASKA_ENABLE_REFCOUNT


  size_t ThreadCache::get_size(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (m == nullptr) {
      return this->runtime.heap.huge_allocator.size_of(handle);
    }

    if (m->is_free()) return 0;
    void *ptr = m->get_pointer();
    auto *page = this->runtime.heap.pt.get_unaligned(ptr);
    if (page == nullptr) return this->runtime.heap.huge_allocator.size_of(ptr);
    return page->size_of(ptr);
  }

  Mapping *ThreadCache::new_mapping(void) {
    auto m = handle_slab->alloc();

    if (unlikely(m == NULL)) {
      auto new_handle_slab = runtime.handle_table.new_slab(this);
      this->handle_slab->set_owner(NULL);
      this->handle_slab = new_handle_slab;
      // This BETTER work!
      m = handle_slab->alloc();
    }

    // Handle 0 is disallowed on yukon hardware because it cannot be invalidated
    // Also, 10 is cursed so we skip it too
    auto hid = m->handle_id();
    if (unlikely(hid == 0 || hid == 10)) {
      return new_mapping();
    }

    return m;
  }


  bool ThreadCache::localize(void *handle, uint64_t epoch) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (unlikely(m == nullptr)) {
      return false;
    }

    return localize(*m, epoch);
  }


  bool ThreadCache::localize(alaska::Mapping &m, uint64_t epoch) {
    if (m.is_pinned() or m.is_free()) return false;

    void *ptr = m.get_pointer();
    auto *source_page = this->runtime.heap.pt.get_unaligned(ptr);

    // Validate that we can indeed move this object from the page.
    if (source_page == nullptr or not source_page->should_localize_from(epoch)) return false;

    // Ask the page for the size of the pointer
    auto size = source_page->size_of(ptr);

    // Arbitrarially block objects larger than 512 from being moved.
    if (size > 512) return false;

    if (locality_page == nullptr or locality_page->available() < size * 2) {
      locality_page = new_locality_page(size + 32);
    }

    // If we are moving an object within the locality page, don't.
    if (unlikely(source_page == locality_page)) return false;

    void *d = locality_page->alloc(m, size);
    locality_page->last_localization_epoch = epoch;
    memcpy(d, ptr, size);
    memset(ptr, 0xFA, size);

    // TODO: invalidate!
    m.set_pointer(d);
    source_page->release_remote(m, ptr);

    return true;
  }

}  // namespace alaska
