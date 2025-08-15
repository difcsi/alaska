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

  LTO_INLINE void *ThreadCache::halloc(size_t size, bool zero) {
    if (unlikely(size == 0)) return NULL;
    // alaska::printf("ThreadCache::halloc size=%zu zero=%d\n", size, zero);

    if (unlikely(alaska::should_be_huge_object(size))) {
      // Allocate the huge allocation.

      void *p = alaska_internal_malloc(size);
      if (zero) memset(p, 0, size);

      return p;
    } else {
      auto *m = this->new_mapping();

      int cls = alaska::size_to_class(size);
      void *ptr;
      SizedPage *page = size_classes[cls];

      if (unlikely(page == nullptr)) page = new_sized_page(cls);
      ptr = page->alloc(*m, size);
      if (unlikely(ptr == nullptr)) {
        // OOM?
        page = new_sized_page(cls);
        ptr = page->alloc(*m, size);
      }

      if (zero) {
        memset(ptr, 0, size);
      }

      m->set_pointer(ptr);
      return m->to_handle(0);
    }
  }



  LTO_INLINE void *ThreadCache::hrealloc(void *handle, size_t new_size) {
    // TODO: There is a race here... I think its okay, as a realloc really should
    // be treated like a UAF, and ideally another thread would not access the handle
    // while it is being reallocated.
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);

    auto original_size = this->get_size(handle);

    void *new_handle = this->halloc(new_size, true);

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

    // alaska::printf("hfree: handle=%p, ptr=%p\n", handle, ptr);
    // alaska::printf("  handle slab: %p\n", handle_slab);
    // alaska::printf("  heap page: %p\n", heap_page);

    heap_page->release_local(*m, ptr);
    handle_slab->release_local(m);

    // bool handle_owned = handle_slab->is_owned_by(this);
    // bool heap_owned = heap_page->is_owned_by(this);
    // if (likely(heap_owned and handle_owned)) {
    //   heap_page->release_local(*m, ptr);
    //   handle_slab->release_local(m);
    //   return;
    // }

    // Now the slow path.

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

#define STUB_ALLOCATES_HANDLES

  // -------------------------------------------------------------- //
  LTO_INLINE void *ThreadCache::malloc(size_t size, bool zero) {
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

    if (zero) memset(ptr, 0, size);
    return ptr;
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
    alaska::Mapping *m;
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
      m = &m_p;
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
    auto new_handle_slab = runtime.handle_table.new_slab(this);
    this->handle_slab->set_owner(NULL);
    this->handle_slab = new_handle_slab;
    // This BETTER work!
    auto m = handle_slab->alloc();
    return m;
  }

  void ThreadCache::free_mapping(alaska::Mapping *m) { this->runtime.handle_table.put(m, this); }


  long ThreadCache::localize(alaska::Mapping *m, long allowed_depth) {
    // alaska::printf("Localizing mapping %p\n", m);
    long moved_count = 0;
    void *ptr = m->get_pointer();
    if (ptr == nullptr || m->is_free() || m->is_pinned()) return 0;
    auto *source_page = this->runtime.heap.pt.get_unaligned(ptr);

    // Validate that we can indeed move this object from the page.
    if (unlikely(source_page == nullptr)) {
      alaska::printf("  Cannot localize pointer %p, not in heap\n", ptr);
      return 0;
    }

    auto header = alaska::ObjectHeader::from(ptr);

    // Ask the page for the size of the pointer
    auto size = header->object_size();

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

    // hotness_hist[header->hotness]--;
    // Now for the actual localization
    memcpy(new_location, ptr, size);       // Copy the data
    source_page->release_remote(*m, ptr);  // Release the old location
    m->set_pointer(new_location);          // Write the new location
    moved_count += 1;

    return moved_count;
  }


  static inline long record_hotness(Mapping *m, uint64_t *hotness_hist, long allowed_depth = 0) {
    void *data = m->get_pointer();
    if (data == nullptr) return 0;

    auto header = ObjectHeader::from(m);

    // An already localized object should not be double localized (yet)
    if (header->localized) return 0;

    if (header->hotness < 0b111'111) {
      if (header->hotness != 0) {
        hotness_hist[header->hotness]--;
      }
      header->hotness++;
      hotness_hist[header->hotness]++;
    }

    return 1;
  }

  ThreadCache::LocalizationResult ThreadCache::localize(alaska::handle_id_t *hids, size_t count) {
    LocalizationResult res;
    res.count = 0;  // We haven't localized anything yet.
    long seen = 0;

    uintptr_t pages[count];
    lphashset_init(pages, count);


    long invalid_found = 0;
    for (size_t i = 0; i < count; i++) {
      if (hids[i] == 0) continue;
      auto *m = Mapping::from_handle_id(hids[i]);

      auto *ptr = m->get_pointer();
      if (ptr != nullptr) {
        lphashset_insert(pages, count, (uintptr_t)m->get_pointer() >> 12);
      }
      record_hotness(m, hotness_hist, 0);
    }

    localization_epoch++;




    return res;
  }



}  // namespace alaska
