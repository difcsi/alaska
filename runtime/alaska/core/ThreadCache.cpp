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

#include <alaska/core/ThreadCache.hpp>
#include <alaska/util/Logger.hpp>
#include <alaska/core/Runtime.hpp>
#include <alaska/heaps/SizeClass.hpp>
#include <alaska/alaska.hpp>
#include "alaska/heaps/Heap.hpp"
#include "alaska/heaps/HeapPage.hpp"
#include <alaska/util/utils.h>
#include <alaska/util/lphash_set.h>
#include <alaska/gc_bitmaps.hpp>
#include <alaska/internal/alaska_internal_malloc.h>

#include <execinfo.h>
#include <stdlib.h>

namespace alaska {

  ThreadCache::ThreadCache(int id, alaska::Runtime &rt)
      : id(id)
      , runtime(rt)
      , localizer(rt.config, *this) {
    for (size_class_t i = 0; i < alaska::num_size_classes; i++) {
      size_classes[i] = nullptr;
    }
    this->current_slab = nullptr;

#if ALASKA_ENABLE_DEFER_RC
    // Preallocate the deferred-RC increment log ONCE (Levanoni-Petrank; see defer_inc /
    // drain_inc). Deferral is a COMPILE-TIME choice (ALASKA_ENABLE_DEFER_RC, the *-defer
    // preset), so this block is absent from an eager build -- no buffer, no hot-path cost.
    // Capacity (entries) from ALASKA_INC_LOG_CAP (default 4M = 32 MiB/thread). On allocation
    // failure inc_log stays null and defer_inc reports overflow -> eager increments.
    const char *e = getenv("ALASKA_INC_LOG_CAP");
    long v = (e != nullptr) ? atol(e) : 0;
    inc_log_cap = (v > 0) ? (size_t)v : (1u << 22);
    inc_log = (alaska::Mapping **)alaska_internal_malloc(inc_log_cap * sizeof(alaska::Mapping *));
#endif
  }

#if ALASKA_ENABLE_DEFER_RC
  // Apply this thread's deferred increments in FIFO order (see defer_inc). Runs
  // world-stopped at the barrier (init.cpp / reclaim / cycle collect) before any free
  // decision reads a count. FIFO, not sorted: address-sorting the log was measured to lose
  // (radix sort cost > locality recovered) in main-rc, so this is a plain replay.
  void ThreadCache::drain_inc(void) {
    uint32_t n = inc_log_head;
    if (n == 0) return;
#if ALASKA_ENABLE_EVENT_COUNTERS
    alaska::events::rc_observe_hwm(n);
#endif
    for (uint32_t i = 0; i < n; i++) {
      alaska::Mapping *m = inc_log[i];
      int new_count = m->inc_refcount();  // dev's inc_refcount already tallies incref
#if ALASKA_ENABLE_CYCLE_COLLECTION
      // Resurrection fixup: a deferred inc lifting a handle off refcount 0 must drop it
      // from the nullcount set so the barrier reclaim does not free a now-live handle.
      if (new_count >= 1) alaska::gc::nullcount_bm_clear(m);
#else
      (void)new_count;
#endif
#if ALASKA_ENABLE_EVENT_COUNTERS
      alaska::events::rc_applied_event(1);  // incref already counted by inc_refcount()
#endif
    }
    inc_log_head = 0;
  }

  ThreadCache::~ThreadCache(void) {
    if (inc_log != nullptr) alaska_internal_free(inc_log);
  }
#endif  // ALASKA_ENABLE_DEFER_RC


#if ALASKA_ENABLE_REUSE_CACHE
  // Reclaim a cached (mapping, backing) entry through the normal free path: release the
  // backing to whatever page now owns it and return the handle slot to the table. Used when
  // the cached page is swapped out or at teardown -- exactly the tail of ThreadCache::hfree.
  void ThreadCache::flush_reuse_class(int cls) {
    ReuseEntry &e = reuse_cache[cls];
    if (e.m == nullptr) return;
    alaska::Mapping *m = e.m;
    void *ptr = e.ptr;
    e = ReuseEntry{};
    auto *heap_page = alaska::Heap::get_page(ptr);
    if (heap_page != nullptr) {
      if (heap_page->is_owned_by(this))
        heap_page->release_local(*m, ptr);
      else
        heap_page->release_remote(*m, ptr);
    }
    m->set_pointer(nullptr);
    this->runtime.handle_table.put(m, this);
  }

  void ThreadCache::flush_reuse_cache(void) {
    for (int cls = 0; cls < alaska::num_size_classes; cls++)
      flush_reuse_class(cls);
  }
#endif


  SizedPage *ThreadCache::new_sized_page(int cls) {
    // alaska::printf("Thread %d needs new sized page for class %d\n", id, cls);
    heap_churn++;
#if ALASKA_ENABLE_REUSE_CACHE
    // The cached entry for this class lives on the page we are about to swap out; reclaim it
    // the normal way before it stops being our current page for the class.
    flush_reuse_class(cls);
#endif
    // Get a new heap
    auto *heap = runtime.heap.get_sizedpage(alaska::class_to_size(cls), this);

    // And set the owner
    heap->set_owner(this);

    // Swap the heaps in the thread cache
    if (size_classes[cls] != nullptr) runtime.heap.put_page(size_classes[cls]);
    // alaska::printf("Thread %d got new sized page %p for class %d\n", id, heap, cls);
    size_classes[cls] = heap;

    ALASKA_ASSERT(heap->available() > 0, "New heap must have space");
    return heap;
  }


  LocalityPage *ThreadCache::new_locality_page(size_t required_size) {
    heap_churn++;
    // Get a new heap
    auto *lp = runtime.heap.get_localitypage(required_size, this);

    // Swap the heaps in the thread cache
    if (this->locality_page != nullptr) runtime.heap.put_page(this->locality_page);
    this->locality_page = lp;

    ALASKA_ASSERT(lp->available() > 0, "New heap must have space");
    return lp;
  }




#define TC_ALIGNED(p) ((__typeof__(p))__builtin_assume_aligned((p), sizeof(uintptr_t)))

  // noinline
  __attribute__((noinline)) void *ThreadCache::halloc_generic(size_t size) {
    // Now, if we are being called here, it means either we are
    // allocating a large object (size>1024) or one of the following
    // checks failed in ::halloc.
    if (unlikely(size == 0)) return NULL;
    void *result = nullptr;

    if (++this->generic_count >= 100) {
      // Minor collect!

      this->generic_collect_count += this->generic_count;
      this->generic_count = 0;

      // Collect every once in a while
      constexpr long generic_collect = 1'000;
      if (this->generic_collect_count >= generic_collect) {
        // Major collect!
        this->generic_collect_count = 0;

        int sc = alaska::size_to_class(size);
        // printf("Major collect on thread %d, sc=%d, size=%zu\n", this->id, sc, size);
        runtime.heap.collect(this, sc);
        // runtime.grade_heap();
      }
    }

    if (likely(size >= alaska::max_large_size)) {
      // alaska::printf("ThreadCache::halloc_generic: huge alloc %zu\n", size);
      result = alaska_internal_malloc(size);
    } else {
      int cls = alaska::size_to_class(size);
      if (cls == 0) return NULL;
      auto *mapping = TC_ALIGNED(this->new_mapping());

      void *ptr;
      SizedPage *page = size_classes[cls];

      if (unlikely(page == nullptr)) page = new_sized_page(cls);
      ptr = TC_ALIGNED(page->alloc(*mapping, size));
      if (unlikely(ptr == nullptr)) {
        // OOM?
        page = new_sized_page(cls);
        ptr = TC_ALIGNED(page->alloc(*mapping, size));
      }


      mapping->set_pointer(ptr);

      result = mapping->to_handle(0);
    }

    // if (zero) memset(result, 0, size);
    return result;
  }


#if 0

#define halloc_track(name) (name++)
  static uint64_t halloc_calls = 0;
  static uint64_t halloc_fastpath = 0;
  static uint64_t halloc_invalid = 0;
  static uint64_t halloc_not_small = 0;
  static uint64_t halloc_no_sp = 0;
  static uint64_t halloc_sp_empty = 0;
  static uint64_t halloc_ht_empty = 0;

  __attribute__((destructor)) void halloc_stats() {
    // print all the stats on their own line, including a percentage of calls to hallo
    alaska::printf("halloc calls: %lu\n", halloc_calls);
    alaska::printf("halloc fastpath: %lu (%.2f%%)\n", halloc_fastpath,
        (halloc_fastpath * 100.0) / halloc_calls);
    alaska::printf(
        "halloc invalid: %lu (%.2f%%)\n", halloc_invalid, (halloc_invalid * 100.0) / halloc_calls);
    alaska::printf("halloc not small: %lu (%.2f%%)\n", halloc_not_small,
        (halloc_not_small * 100.0) / halloc_calls);
    alaska::printf(
        "halloc no sp: %lu (%.2f%%)\n", halloc_no_sp, (halloc_no_sp * 100.0) / halloc_calls);
    alaska::printf("halloc sp_empty: %lu (%.2f%%)\n", halloc_sp_empty,
        (halloc_sp_empty * 100.0) / halloc_calls);
    alaska::printf("halloc ht_empty: %lu (%.2f%%)\n", halloc_ht_empty,
        (halloc_ht_empty * 100.0) / halloc_calls);
  }


#else
#define halloc_track(name)
#endif

  // A version of halloc which uses the global domain.
  LTO_INLINE void *ThreadCache::halloc(size_t size) {
    halloc_track(halloc_calls);
    int cls = alaska::size_to_class(size);
    if (cls == 0) {
      halloc_track(halloc_invalid);
      return NULL;
    }

#if ALASKA_ENABLE_REUSE_CACHE
    // Perceus fast path: re-hand the (mapping, backing) a uniquely-owned free stashed for
    // this size class, skipping the freelist round-trip. Only if the cached page is still
    // this tc's current page for the class (else the entry is stale -> flush it). The backing
    // is returned uninitialized (halloc semantics); hcalloc's zeroing runs at the rt level.
    {
      ReuseEntry &e = reuse_cache[cls];
      if (e.m != nullptr) {
        if (e.page == size_classes[cls]) {
          alaska::Mapping *m = e.m;
          void *ptr = e.ptr;
          e = ReuseEntry{};  // consume
          m->reset();        // refcount -> 0, flags cleared (matches a fresh mapping)
          auto *header = alaska::ObjectHeader::from(ptr);
          header->set_mapping(m);
          header->set_object_size(size);
          m->set_pointer(header->data());  // == ptr
          halloc_track(halloc_fastpath);
          return m->to_handle(0);
        }
        flush_reuse_class(cls);  // stale (page swapped out): free normally, fall through
      }
    }
#endif

    if (likely(size < alaska::max_small_size)) {
      // Grab the sized page for this size class.
      auto *sp = size_classes[cls];

      if (likely(sp != NULL)) {
        auto *slab = this->current_slab;
        if (unlikely(slab == nullptr)) {
          // Need to get a slab first
          return halloc_generic(size);
        }
        auto &htfl = slab->get_freelist();
        auto &spfl = sp->get_freelist();

        // Peek at the handle table and size page free lists.
        auto *m = TC_ALIGNED(htfl.peek());
        auto *d = TC_ALIGNED(spfl.peek());

        if (m == nullptr) halloc_track(halloc_ht_empty);
        if (d == nullptr) halloc_track(halloc_sp_empty);

        // If both had local free entries, we can take the fast path.
        if (likely((!!m) & (!!d))) {
          // Pop from both free lists.
          htfl.pop_unchecked(m);
          spfl.pop_unchecked(d);

          // Setup the handle table mapping.
          auto *mapping = (alaska::Mapping *)m;
          auto *header = &d->header;
          header->set_mapping(mapping);
          header->set_object_size(size);
          mapping->set_pointer(header->data());

          halloc_track(halloc_fastpath);
          return mapping->to_handle(0);
        }
      } else
        halloc_track(halloc_no_sp);
    } else
      halloc_track(halloc_not_small);

    // Ope! Fallback to the slower generic path.
    return halloc_generic(size);
  }




  LTO_INLINE void *ThreadCache::hrealloc(void *handle, size_t new_size) {
    // TODO: There is a race here... I think its okay, as a realloc really should
    // be treated like a UAF, and ideally another thread would not access the handle
    // while it is being reallocated.
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);

    auto original_size = this->get_size(handle);

    void *new_handle = this->halloc(new_size);
    
    // We should copy the minimum of the two sizes between the allocations.
    size_t copy_size = original_size > new_size ? new_size : original_size;

    handle_memcpy(new_handle, handle, copy_size);

    //new_handle->copy_refcount_from(handle);

    alaska::printf("User called realloc, ignore next free warning\n");
    this->hfree(handle);

    return new_handle;
  }



  LTO_INLINE void ThreadCache::hfree(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);

    // The first case in hfree is handling huge allocations.
    // These allocations are not tracked in the handle table, so we
    // need to handle them by calling out to the huge allocator.
    // This is behind an unlikely check because it is expected that these
    // are relatively rare.

    if (unlikely(m == nullptr)) {
      alaska_internal_free(handle);
      return;
    }

    // --- Free the data allocation --- //
    void *ptr = m->get_pointer();
    // auto *header = alaska::ObjectHeader::from(ptr);

    auto *handle_slab = this->runtime.handle_table.get_slab(m);
    auto *heap_page = alaska::Heap::get_page(ptr);

    bool heap_owned = heap_page->is_owned_by(this);

#if ALASKA_ENABLE_REUSE_CACHE
    // Perceus stash: a uniquely-owned (refcount <= 1), locally-owned handle freed here is the
    // only live reference, so its slot+backing can be re-handed to the next same-size alloc
    // without aliasing a survivor. Stash the (mapping, backing, page) triple instead of freeing
    // it -- iff the page is still this tc's current page for the object's class and the per-class
    // slot is empty (single-entry cache; else fall through to the normal free). The GC
    // nullcount/candidate sets were already cleared for this handle by the global hfree wrapper
    // (rt/halloc.cpp), so a stashed live slot cannot be reclaimed underneath us.
    if (heap_owned && m->get_refcount() <= 1 && !m->is_pinned()) {
      auto *rheader = alaska::ObjectHeader::from(ptr);
      int rcls = alaska::size_to_class(rheader->object_size());
      if (rcls > 0 && rcls < alaska::num_size_classes &&
          (alaska::HeapPage *)size_classes[rcls] == heap_page && reuse_cache[rcls].m == nullptr) {
        reuse_cache[rcls] = ReuseEntry{m, ptr, size_classes[rcls]};
        return;
      }
    }
#endif

    // Now the slow path.

    if(unlikely(m->get_refcount() > 0 )){
      alaska::printf("Warning: Freeing handle %p with non-zero refcount %lu\n", handle, m->get_refcount());
    }


    if (likely(heap_owned)) {
      heap_page->release_local(*m, ptr);
    } else {
      heap_page->release_remote(*m, ptr);
    }

    // Return the handle to the slab using thread-safe atomic operations.
    // This works correctly even if freed from a different thread than allocation.
    handle_slab->free(m);
    return;
  }

  // #define STUB_ALLOCATES_HANDLES


  LTO_INLINE alaska::Mapping *ThreadCache::reverse_lookup(void *heap_ptr) {
    if (this->runtime.heap.contains(heap_ptr)) {
      return ObjectHeader::from(heap_ptr)->get_mapping();
    }
    return nullptr;
  }

  // -------------------------------------------------------------- //
  LTO_INLINE void *ThreadCache::malloc_generic(size_t size) {
    void *ptr;
    if (unlikely(alaska::should_be_huge_object(size))) {
      // Allocate the huge allocation.
      ptr = alaska_internal_malloc(size);
    } else {
      int cls = alaska::size_to_class(size);
      SizedPage *page = size_classes[cls];
      if (unlikely(page == nullptr)) page = new_sized_page(cls);

      alaska::Mapping *m;

#ifdef STUB_ALLOCATES_HANDLES
      m = this->new_mapping();
#else
      // Stub Mapping
      alaska::Mapping m_p{};
      m = &m_p;
#endif

      ptr = page->alloc(*m, size);
      if (unlikely(ptr == nullptr)) {
        // OOM?
        page = new_sized_page(cls);
        ptr = page->alloc(*m, size);
      }
      m->set_pointer(ptr);
    }

    return ptr;
  }


  LTO_INLINE void *ThreadCache::malloc(size_t size, bool zero_ignored) {
    if (likely(size < alaska::max_small_size)) {
      // int cls = alaska::size_to_class(size);
      auto *sp = size_classes[alaska::size_to_class_small(size)];

      // alaska::printf("sp=%p\n", sp);

      if (likely(sp != NULL)) {
        auto *slab = this->current_slab;
        if (unlikely(slab == nullptr)) {
          // Need to get a slab first
          return halloc_generic(size);
        }
        auto &htfl = slab->get_freelist();
        auto &spfl = sp->get_freelist();
// Note: we cram an ALIGNED here because the RISCV compiler
// can't figure it out, and emits four loads and four stores
// to avoid alignment problems. If we tell the compiler it is
// aligned, things get much faster (It should be aligned anyways)
#ifdef STUB_ALLOCATES_HANDLES
        auto *m = TC_ALIGNED(htfl.peek());  // peek at the handle table
#else
        // Stub mapping
        alaska::Mapping m_p{};
        auto *m = &m_p;
#endif
        auto *d = TC_ALIGNED(spfl.peek());  // peek at the size page
        // alaska::printf("m=%p, d=%p\n", m, d);

        // If both had local free entries, we can take the fast path.
        if (likely(m and d)) {
// Pop from both free lists.
#ifdef STUB_ALLOCATES_HANDLES
          htfl.pop(m);
#endif
          spfl.pop(d);


          // Setup the handle table mapping.
          auto *mapping = (alaska::Mapping *)m;
          auto *header = &d->header;
          header->set_mapping(mapping);
          header->set_object_size(size);
          void *data = header->data();
          mapping->set_pointer(data);

          return data;
        }
      }
    }

    // Ope! Fallback to the slower generic path.
    return malloc_generic(size);
  }


  LTO_INLINE void *ThreadCache::realloc(void *ptr, size_t new_size) {
    auto original_size = this->get_size(ptr);
    void *new_ptr = this->malloc(new_size, true);
    // We should copy the minimum of the two sizes between the allocations.
    size_t copy_size = original_size > new_size ? new_size : original_size;
    handle_memcpy(new_ptr, ptr, copy_size);
    this->free(ptr);
    return new_ptr;
  }



  LTO_INLINE void ThreadCache::free(void *ptr) {
    if (this->runtime.heap.contains(ptr)) {
      auto *heap_page = alaska::Heap::get_page(ptr);


#ifdef STUB_ALLOCATES_HANDLES
      alaska::Mapping *m = ObjectHeader::from(ptr)->get_mapping();
      // Release the mapping

      auto *handle_slab = this->runtime.handle_table.get_slab(m);
      // Use thread-safe atomic free operation
      handle_slab->free(m);
#else
      // Stub mapping
      alaska::Mapping m_p{};
      auto *m = &m_p;
#endif

      heap_page->release_local(*m, ptr);
    } else {
      alaska_internal_free(ptr);
    }
  }

  // -------------------------------------------------------------- //


  LTO_INLINE size_t ThreadCache::get_size(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (m == nullptr) {
      void *pointer = handle;

      if (runtime.heap.contains(pointer)) {
        // it has an object header.
        return alaska::ObjectHeader::from(pointer)->object_size();
      } else {
        return alaska_internal_malloc_usable_size(pointer);
      }
    }


    if (m->is_free()) return 0;

    void *ptr = m->get_pointer();
    auto header = alaska::ObjectHeader::from(ptr);
    return header->object_size();
  }

  __attribute__((noinline)) Mapping *ThreadCache::new_mapping_slow_path(void) {
    handle_table_churn++;  // record that we are looking for a new handle table slab.
    // printf("Ran out of handles in the slab!\n");

    // We need to get a new slab from the handle table
    auto new_slab = runtime.handle_table.fresh_slab();

    if (new_slab == nullptr) {
      return nullptr;
    }

    if (current_slab != nullptr) {
      current_slab->schedule_deferred();
    }

    // Update our current slab
    this->current_slab = new_slab;

    // Allocate from the new slab
    auto m = new_slab->alloc();
    return m;
  }

  void ThreadCache::free_mapping(alaska::Mapping *m) { this->runtime.handle_table.put(m, this); }



  constexpr long required_size_for_new_locality_page = 4096;
  long ThreadCache::localize_one(alaska::Mapping *m) {
    bool localized = locality_page->localize(*m);

    if (!localized) {
      // We failed to localize, so we need a new locality page.
      locality_page =
          new_locality_page(required_size_for_new_locality_page);  // Make it big enough for a page.
      localized = locality_page->localize(*m);
    }
    return localized ? 1 : 0;
  }

  long ThreadCache::localize(alaska::Mapping *m, long allowed_depth, long depth) {
    if (m->is_free() || m->is_pinned()) return 0;

    if (unlikely(locality_page == nullptr))
      locality_page = new_locality_page(required_size_for_new_locality_page);

    void *old_data = m->get_pointer();
    auto *old_header = alaska::ObjectHeader::from(old_data);


    size_t object_size = old_header->object_size();

    if (object_size > 512 || old_header->localized) {
      return 0;
    }

    bool localized = false;
    localized += localize_one(m);

    if (allowed_depth == 1) return localized;

    long num_recursed = 0;
    auto header = alaska::ObjectHeader::from(m);

    // alaska::Mapping *to_walk[object_size / sizeof(void *)];
    // long walk_top = 0;


    header->walk([&](alaska::Mapping *om, alaska::ObjectHeader *oheader) {
      // to_walk[walk_top++] = om;
      localized += localize_one(om);
      num_recursed += this->localize(om, allowed_depth - 1, depth + 1);
    });

    // for (long i = 0; i < walk_top; i++) {
    //   num_recursed += this->localize(to_walk[i], allowed_depth - 1, depth + 1);
    // }


    return (localized ? 1 : 0) + num_recursed;
  }

  ThreadCache::LocalizationResult ThreadCache::localize(alaska::handle_id_t *hids, size_t count) {
    static int dump_number = 0;
    int dump = dump_number++;
    LocalizationResult res;
    res.count = 0;  // We haven't localized anything yet.


    runtime.with_barrier([&]() {
      LocalityReport report;
      long localizedHandles = 0;

      size_t walk_distance = 1;
      walk_distance = count;
      int num_duplicates = 0;

      for (size_t i = 0; i < walk_distance; i++) {
        alaska::Mapping *m = nullptr;
        void *data = nullptr;

        auto hid = hids[i];
        if (hid == 0) continue;
        if (!alaska::check_mapping(hid, m, data)) continue;

        // check that the handle is unique in the hids list.
        // bool unique = true;
        // for (size_t j = 0; j < i; j++) {
        //   if (hids[j] == hid) {
        //     unique = false;
        //     num_duplicates++;
        //     break;
        //   }
        // }
        // if (!unique) continue;
        constexpr int depth = 16;

        // alaska::grade_locality(*m, depth, report);
        localizedHandles += localize(m, depth);
      }
      // alaska::printf("dump %d had %d duplicate handles\n", dump, num_duplicates);

      // report.dump();
      // alaska::printf("YUKON_LOCALITY: %6.2f%%, %zuB %fMB, %ld localized\n",
      //     report.locality() * 100.0f, report.object_bytes,
      //     report.object_bytes / (1024.0f * 1024.0f), localizedHandles);

      res.count = localizedHandles;
    });

    return res;
  }


  static __thread alaska::ThreadCache *g_tc = nullptr;

  alaska::ThreadCache *alaska::ThreadCache::current() {
    if (g_tc == nullptr) {
      auto &rt = alaska::Runtime::get();
      g_tc = rt.new_threadcache();
    }
    return g_tc;
  }

}  // namespace alaska
