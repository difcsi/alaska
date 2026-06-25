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

#pragma once

#include <alaska/Heap.hpp>
#include <alaska/HeapPage.hpp>
#include <alaska/HandleTable.hpp>
#include <alaska/LocalityPage.hpp>
#include <alaska/alaska.hpp>
#include <alaska/Localizer.hpp>
#include "ck/lock.h"
#include <ck/vec.h>

namespace alaska {

  struct Runtime;

  // A ThreadCache is the class which manages the thread-private
  // allocations out of a shared heap. The core runtime itself does
  // *not* manage where thread caches are used. It is assumed
  // something else manages storing a pointer to a ThreadCache in some
  // thread-local variable
  class ThreadCache final : public alaska::InternalHeapAllocated {
   public:
    ThreadCache(int id, alaska::Runtime &rt);

    void *halloc(size_t size, bool zero = false);
    void *hrealloc(void *handle, size_t new_size);
    void hfree(void *handle);

#if ALASKA_ENABLE_REFCOUNT
    // Deferred dec-on-free (the default; opt out with ALASKA_NO_FREE_DEC_DEFER): instead of
    // running the expensive alaska_hfree_dec_children scan + backing free on the freeing
    // thread's hot path, hfree enqueues the handle via defer_free and the barrier thread
    // drains the queue via drain_deferred (world stopped). BOTH methods assume the caller
    // already holds this->lock -- the get_tc() LockedThreadCache on the mutator enqueue path,
    // and lock_all_thread_caches() on the barrier drain path.
    void defer_free(void *handle);
    void drain_deferred(void);
    // Release the handle slots the deferred-drain quarantine withheld one barrier epoch
    // ago, and roll the current epoch's quarantine forward. Called once per barrier AFTER
    // every thread's deferred queue has drained (see hfree_impl / quarantine_rotate).
    void quarantine_rotate(void);
#endif

    int get_id(void) const { return this->id; }
    size_t get_size(void *handle);


    bool localize(alaska::Mapping &m, uint64_t epoch);
    bool localize(void *handle, uint64_t epoch);


   protected:
    friend class LockedThreadCache;
    friend alaska::Runtime;
    friend alaska::Localizer;

    ck::mutex lock;

    // Allocate backing data for a handle, but don't assign it yet.
    void *allocate_backing_data(const alaska::Mapping &m, size_t size);

    // Free an allocation behind a handle, but not the handle
    void free_allocation(const alaska::Mapping &m);

    // Shared body of hfree. When `quarantine_slot` is true (the deferred-drain path) the
    // backing is freed and the pointer cleared, but the mapping slot is withheld from the
    // allocatable pool for one barrier epoch instead of being put back immediately.
    void hfree_impl(void *handle, bool quarantine_slot);

    // Allocate a new handle table mapping
    alaska::Mapping *new_mapping(void);
    // Swap to a new sized page owned by this thread cache
    alaska::SizedPage *new_sized_page(int cls);

#if ALASKA_ENABLE_REFCOUNT
    // Perceus-flavored reuse cache (needs the refcount; inert on the default deferred-free
    // path). One slot per size class: when a uniquely-owned handle (refcount <= 1) is freed
    // on the SYNCHRONOUS path, hfree_impl stashes its still-live (mapping, backing) pair here
    // instead of returning the backing to the freelist and the slot to the handle table.
    // The next same-size halloc re-hands the same handle + backing, skipping the whole
    // round-trip (the page Header still points at the mapping). `page` is recorded so the
    // alloc path can verify the page is still this tc's current page for the class before
    // reusing (a swapped-out page invalidates the entry).
    struct ReuseEntry {
      alaska::Mapping *m = nullptr;
      void *ptr = nullptr;
      alaska::SizedPage *page = nullptr;
    };
    ReuseEntry reuse_cache[alaska::num_size_classes];

    // Drop a cached entry back through the normal free path (free backing + return slot).
    void flush_reuse_class(int cls);
    // Flush every cached entry (page swap-out / thread-cache teardown).
    void flush_reuse_cache(void);
#endif
    // Swap to a new locality page owned by this thread cache
    alaska::LocalityPage *new_locality_page(size_t required_size);

    // Just an id for this thread cache assigned by the runtime upon creation. It's mostly
    // meaningless, meant for debugging.
    int id;
    // A reference to the global runtime. This is here mainly to gain
    // access to the HandleTable and the Heap.
    alaska::Runtime &runtime;

    // A pointer to the current slab of the handle table that this thread
    // cache is allocating from.
    alaska::HandleSlab *handle_slab;

    // Each thread cache has a private heap page for each size class
    // it might allocate from. When a size class fills up, it is
    // returned to the global heap and another one is allocated.
    alaska::SizedPage *size_classes[alaska::num_size_classes] = {nullptr};
    // Each thread cache also has a private "Locality Page", which
    // objects can be relocated to according to some external
    // policy. This page is special because it can contain many
    // objects of many different sizes.
    alaska::LocalityPage *locality_page = nullptr;

#if ALASKA_ENABLE_REFCOUNT
    // Handles awaiting deferred dec-on-free, with the running total of their backing
    // bytes for the byte-cap backpressure check. Guarded by `lock` (see defer_free).
    ck::vec<void *> deferred_frees;
    size_t deferred_bytes = 0;

    // Handle-slot quarantine for the deferred-drain path. A slot whose backing was freed
    // during drain_deferred is parked here (NOT returned to the allocatable pool) until a
    // later barrier, so a not-yet-drained parent's stale child word cannot alias a reused
    // slot and have dec-on-free corrupt the new occupant (the binarytrees UAF). Two
    // generations: `cur` collects this epoch's withheld slots; `prev` holds last epoch's
    // and is released at the next quarantine_rotate.
    ck::vec<alaska::Mapping *> quarantine_cur;
    ck::vec<alaska::Mapping *> quarantine_prev;
#endif

   public:
    // Each thread cache has a localizer, which can be fed with
    // "localization data" to improve object locality
    alaska::Localizer localizer;
  };



  class LockedThreadCache final {
   public:
    LockedThreadCache(ThreadCache &tc)
        : tc(tc) {
      tc.lock.lock();
    }


    ~LockedThreadCache(void) { tc.lock.unlock(); }

    // Delete copy constructor and copy assignment operator
    LockedThreadCache(const LockedThreadCache &) = delete;
    LockedThreadCache &operator=(const LockedThreadCache &) = delete;

    // Delete move constructor and move assignment operator
    LockedThreadCache(LockedThreadCache &&) = delete;
    LockedThreadCache &operator=(LockedThreadCache &&) = delete;




    ThreadCache &operator*(void) { return tc; }
    ThreadCache *operator->(void) { return &tc; }

   private:
    ThreadCache &tc;
  };



}  // namespace alaska
