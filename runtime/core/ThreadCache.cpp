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
#include <alaska/lphash_set.h>

#include <execinfo.h>

namespace alaska {

  ThreadCache::ThreadCache(int id, alaska::Runtime &rt)
      : id(id)
      , runtime(rt)
      , localizer(rt.config, *this) {
    for (size_class_t i = 0; i < alaska::num_size_classes; i++) {
      size_classes[i] = nullptr;
    }
    handle_table_churn++;
    handle_slab = runtime.handle_table.new_slab(this);
  }




  SizedPage *ThreadCache::new_sized_page(int cls) {
    heap_churn++;
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

  LTO_INLINE void *ThreadCache::halloc(size_t size) {
    halloc_track(halloc_calls);
    int cls = alaska::size_to_class(size);
    if (cls == 0) {
      halloc_track(halloc_invalid);
      return NULL;
    }

    if (likely(size < alaska::max_small_size)) {
      // Grab the sized page for this size class.
      auto *sp = size_classes[cls];

      if (likely(sp != NULL)) {
        auto &htfl = handle_slab->get_freelist();
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
    auto *heap_page = this->runtime.heap.pt.get_unaligned(ptr);
    heap_page->release_local(*m, ptr);
    handle_slab->release_local(m);

    // bool handle_owned = handle_slab->is_owned_by(this);
    // bool heap_owned = heap_page->is_owned_by(this);
    // if (likely(heap_owned and handle_owned)) {
    //   heap_page->release_local(*m, ptr);
    //   handle_slab->release_local(m);
    //   return;
    // }

    // // Now the slow path.

    // if (likely(heap_page->is_owned_by(this))) {
    //   heap_page->release_local(*m, ptr);
    // } else {
    //   heap_page->release_remote(*m, ptr);
    // }


    // if (likely(handle_slab->is_owned_by(this))) {
    //   handle_slab->release_local(m);
    // } else {
    //   handle_slab->release_remote(m);
    // }
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
        auto &htfl = handle_slab->get_freelist();
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
      auto *heap_page = this->runtime.heap.pt.get_unaligned(ptr);


#ifdef STUB_ALLOCATES_HANDLES
      alaska::Mapping *m = ObjectHeader::from(ptr)->get_mapping();
      // Release the mapping

      auto *handle_slab = this->runtime.handle_table.get_slab(m);
      if (likely(handle_slab->is_owned_by(this))) {
        handle_slab->release_local(m);
      } else {
        handle_slab->release_remote(m);
      }
#else
      // Stub mapping
      alaska::Mapping m_p{};
      auto *m = &m_p;
#endif

      heap_page->release_local(*m, ptr);
      // if (heap_page->is_owned_by(this)) {
      //   // If the heap page is owned by this thread, we can free it locally.
      //   heap_page->release_local(*m, ptr);
      // } else {
      //   // Otherwise, we need to release it remotely.
      //   heap_page->release_remote(*m, ptr);
      // }
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
    auto new_handle_slab = runtime.handle_table.new_slab(this);
    this->handle_slab->set_owner(NULL);
    this->handle_slab = new_handle_slab;
    // This BETTER work!
    auto m = handle_slab->alloc();
    return m;
  }

  void ThreadCache::free_mapping(alaska::Mapping *m) { this->runtime.handle_table.put(m, this); }



  static inline bool check_handle(void *p, alaska::Mapping *&m, void *&data) {
    m = alaska::Mapping::from_handle_safe(p);
    if (m == nullptr) return false;
    data = m->get_pointer();
    if (data == nullptr || m->is_free()) return false;
    return true;
  }

  static inline bool check_handle(handle_id_t hid, alaska::Mapping *&m, void *&data) {
    if (hid == 0) return false;
    m = alaska::Mapping::from_handle_id(hid);
    if (m == nullptr) return false;
    data = m->get_pointer();
    if (data == nullptr || m->is_free()) return false;
    return true;
  }


  long ThreadCache::localize(alaska::Mapping *m, long allowed_depth) {
    constexpr long max_object_size = 256;
    long moved_count = 0;
    // alaska::printf("Localizing mapping %p -> %p\n", m, m->get_pointer());
    void *ptr = m->get_pointer();
    if (ptr == nullptr || m->is_free() || m->is_pinned()) return 0;

    auto *source_page = this->runtime.heap.pt.get_unaligned(ptr);
    auto header = alaska::ObjectHeader::from(ptr);


    // Ask the page for the size of the pointer
    auto size = header->object_size();
    if (size > max_object_size) return 0;

    if (locality_page == nullptr) {
      alaska::printf("locality_page is null, creating a new one\n");
      locality_page = new_locality_page(size + 32);
    }

    void *new_location = locality_page->alloc(*m, size);
    if (unlikely(new_location == nullptr)) {
      alaska::printf("  Locality page %p cannot allocate %zu bytes\n", locality_page, size);
      locality_page = new_locality_page(size + 32);
      new_location = locality_page->alloc(*m, size);
      if (new_location == nullptr) {
        return 0;
      }
    }

    // Now for the actual localization
    memcpy(new_location, ptr, size);  // Copy the data
    if (source_page->is_owned_by(this))
      source_page->release_local(*m, ptr);  // Release the old location
    else
      source_page->release_remote(*m, ptr);  // Release the old location

    m->set_pointer(new_location);  // Write the new location
    moved_count += 1;

#if 0
    // TODO: localize one level of pointers?
    if (allowed_depth > 0) {
      auto *ptr = (void **)new_location;
      size_t scan_size = 128;
      if (size < scan_size) scan_size = size;
      auto *end = (void **)((char *)ptr + scan_size);

      while (ptr < end) {
        auto *p = *ptr;

        alaska::Mapping *mp;
        void *data;
        if (check_handle(p, mp, data)) {
          auto header = alaska::ObjectHeader::from(data);
          if (not header->localized && header->object_size() <= max_object_size) {
            moved_count += localize(m, allowed_depth - 1);
          }
        }
        ptr++;
      }
    }
#endif
    return moved_count;
  }

  ThreadCache::LocalizationResult ThreadCache::localize(alaska::handle_id_t *hids, size_t count) {
    LocalizationResult res;
    res.count = 0;  // We haven't localized anything yet.

    runtime.with_barrier([&]() {
      long localizedHandles = 0;
      for (size_t i = 0; i < count; i++) {
        alaska::Mapping *m = nullptr;
        void *data = nullptr;
        if (!check_handle(hids[i], m, data)) continue;
        localizedHandles += localize(m, 0);  // Localize to a depth of four.
      }

      res.count = localizedHandles;
    });

    return res;
  }



}  // namespace alaska
