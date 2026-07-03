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

#include <alaska/heaps/Heap.hpp>
#include <alaska/heaps/HeapPage.hpp>
#include <alaska/handles/HandleTable.hpp>

#include <alaska/heaps/LocalityPage.hpp>
#include <alaska/alaska.hpp>
#include <alaska/Localizer.hpp>
#include <alaska/EventCounters.hpp>
#include "ck/lock.h"
#include <alaska/util/RateCounter.hpp>

namespace alaska {

  struct Runtime;

  // A ThreadCache is the class which manages the thread-private
  // allocations out of a shared heap. The core runtime itself does
  // *not* manage where thread caches are used. It is assumed
  // something else manages storing a pointer to a ThreadCache in some
  // thread-local variable
  class ThreadCache final : public alaska::PersistentAllocation {
   protected:
    friend class LockedThreadCache;
    friend alaska::Runtime;
    friend alaska::Localizer;
    friend alaska::HeapPage;

    // Just an id for this thread cache assigned by the runtime upon creation. It's mostly
    // meaningless, meant for debugging.
    int id;
    // A reference to the global runtime. This is here mainly to gain
    // access to the HandleTable and the Heap.
    alaska::Runtime &runtime;

    // Each ThreadCache now manages its own handle slab directly.
    // When the current slab is exhausted, the ThreadCache requests a new one from the HandleTable.
    alaska::HandleSlab *current_slab = nullptr;

    // Each thread cache has a private heap page for each size class
    // it might allocate from. When a size class fills up, it is
    // returned to the global heap and another one is allocated.
    alaska::SizedPage *size_classes[alaska::num_size_classes];

    // Each thread cache also has a private "Locality Page", which
    // objects can be relocated to according to some external
    // policy. This page is special because it can contain many
    // objects of many different sizes.
    alaska::LocalityPage *locality_page = nullptr;

    // How many calls to the generic allocator have we had since the last 'collection'?
    long generic_count = 0;
    long generic_collect_count = 0;

#if ALASKA_ENABLE_REUSE_CACHE
    // Perceus-flavored reuse cache (see CMakeLists ALASKA_ENABLE_REUSE_CACHE). One slot per
    // size class: when a uniquely-owned handle (refcount <= 1) is freed on the synchronous
    // path, hfree stashes its still-live (mapping, backing, page) triple here instead of
    // returning the backing to the page freelist and the slot to the handle table. The next
    // same-size halloc re-hands the same handle + backing, skipping the round-trip. `page` is
    // recorded so the alloc path can verify it is still this tc's current page for the class
    // (a swapped-out page invalidates the entry). Adapted to dev's ObjectHeader allocator.
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

#if ALASKA_ENABLE_DEFER_RC
    // Deferred-RC increment log (Levanoni-Petrank; see defer_inc / drain_inc). A fixed,
    // preallocated single-producer buffer of target mappings awaiting a batched refcount
    // add. NOT a ck::vec: the hot-path append must be malloc-free / async-signal-safe.
    // Allocated once in the ctor; if that fails inc_log stays null and defer_inc always
    // reports overflow (clean degrade to eager increments). Capacity from ALASKA_INC_LOG_CAP.
    size_t inc_log_cap = 0;
    alaska::Mapping **inc_log = nullptr;  // entries [0, inc_log_head) pending
    uint32_t inc_log_head = 0;            // append/commit cursor
#endif


   public:
    // A lock which is used to control access to this heap page. Mostly used to control
    // race conditions around barriers, as the rest of the heap can only be accessed through
    // a locked thread cache as a mediator
    ck::mutex lock;


    // Track allocation and free rates
    alaska::RateCounter allocation_rate;
    alaska::RateCounter free_rate;


    // How often are we getting a new heap or handle table?
    alaska::RateCounter heap_churn;
    alaska::RateCounter handle_table_churn;

    // Each thread cache has a localizer, which can be fed with
    // "localization data" to improve object locality
    alaska::Localizer localizer;

   public:
    ThreadCache(int id, alaska::Runtime &rt);

#if ALASKA_ENABLE_DEFER_RC
    ~ThreadCache(void);

    // Append a deferred increment (the hot-path increment barrier routes here in a
    // deferred build). Returns false on overflow / no log so the caller falls back to
    // an eager increment -- correctness never depends on the buffer having room. Single
    // -producer, lock-free: its sole consumer drain_inc runs world-stopped at the barrier.
    bool defer_inc(alaska::Mapping *m) {
      uint32_t h = inc_log_head;
      if (unlikely(inc_log == nullptr || h >= inc_log_cap)) {
#if ALASKA_ENABLE_EVENT_COUNTERS
        alaska::events::rc_overflow_event();
#endif
        return false;
      }
      inc_log[h] = m;        // (1) write the slot, THEN
      inc_log_head = h + 1;  // (2) publish it (the stop-the-world barrier is the fence).
#if ALASKA_ENABLE_EVENT_COUNTERS
      alaska::events::rc_deferred_event();
#endif
      return true;
    }

    // Apply this thread's deferred increments in FIFO order. Caller must run world
    // -stopped (barrier) or hold this->lock. MUST precede any dec-on-free / reclaim
    // (the Levanoni-Petrank INV-LIVE linchpin: a child with a pending inc must be back
    // at full count before a free decision reads it).
    void drain_inc(void);
#endif

    // Handle allocation and deallocation routines.
    void *halloc(size_t size) alaska_attr_malloc;

    void *halloc_generic(size_t size) alaska_attr_malloc;


    void *hrealloc(void *handle, size_t new_size) alaska_attr_malloc;
    void hfree(void *handle);


    // Non-handle allocation and deallocation routines.
    //    These routines are for 'baseline' measurements with our allocator, and
    //    shows the overhead of using handles with the same underlying
    //    allocator.
    // You SHOULD NOT use this function *and* the handle allocation routine
    // in the same execution context, as it will likely cause bugs.
    void *malloc_generic(size_t size) alaska_attr_malloc;
    void *malloc(size_t size, bool zero = false) alaska_attr_malloc;
    void *realloc(void *ptr, size_t new_size) alaska_attr_malloc;
    void free(void *ptr);


    int get_id(void) const { return this->id; }
    size_t get_size(void *handle);


    struct LocalizationResult {
      size_t count;  // how many mappings were localized
    };
    // The thread cache is responsible for localizing a set of mappings to improve object
    // locality. This function takes a list of ordered mappings and lays them out contiguously
    // in memory and the mappings are updated to point to their new locations.
    // This function is called from the Localizer class.
    LocalizationResult localize(alaska::handle_id_t *hids, size_t count);
    static constexpr uint64_t hotness_hist_size = 1 << 6;
    uint64_t localization_epoch = 0;
    uint64_t hotness_hist[hotness_hist_size] = {0};
    long localize(alaska::Mapping *mapping, long allowed_depth = 0, long depth = 0);
    long localize_one(alaska::Mapping *mapping);

    // Allocate a new handle table mapping
    alaska::Mapping *new_mapping(void);
    alaska::Mapping *new_mapping_slow_path(void);
    void free_mapping(alaska::Mapping *);

    static ThreadCache *current(void);

   private:
    alaska::Mapping *reverse_lookup(void *heap_ptr);

    // Swap to a new sized page owned by this thread cache
    alaska::SizedPage *new_sized_page(int cls);
    // Swap to a new locality page owned by this thread cache
    alaska::LocalityPage *new_locality_page(size_t required_size);
  };


  inline alaska::Mapping *ThreadCache::new_mapping(void) {
    if (unlikely(current_slab == nullptr || !current_slab->has_any_free())) {
      // Slow path: find/allocate a new slab
      return new_mapping_slow_path();
    }
    auto m = current_slab->alloc();
    if (unlikely(m == nullptr)) {
      // Slab appeared to have space but allocation failed, try slow path
      return new_mapping_slow_path();
    }
    return m;
  }




  class LockedThreadCache final {
   public:
    LockedThreadCache(ThreadCache &tc)
        : tc(tc) {
      // tc.lock.lock();
    }


    ~LockedThreadCache(void) {
      // tc.lock.unlock();
    }

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
