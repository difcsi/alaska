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

// #define USE_MALLOC
#ifdef USE_MALLOC
#include <malloc.h>
#endif



namespace alaska {



  ThreadCache::ThreadCache(int id, alaska::Runtime &rt)
      : id(id)
      , runtime(rt)
      , localizer(rt.config, *this) {
    handle_slab = runtime.handle_table.new_slab(this);
  }


  void *ThreadCache::allocate_backing_data(const alaska::Mapping &m, size_t size) {
    int cls = alaska::size_to_class(size);
    this->allocation_rate.track(1);
    void *ptr;
#ifdef USE_MALLOC
    ptr = ::malloc(size);
#else
    SizedPage *page = size_classes[cls];
    if (unlikely(page == nullptr)) page = new_sized_page(cls);
    ptr = page->alloc(m, size);
    if (unlikely(ptr == nullptr)) {
      // OOM?
      page = new_sized_page(cls);
      ptr = page->alloc(m, size);
      ALASKA_ASSERT(ptr != nullptr, "OOM!");
    }
#endif
    return ptr;
  }




  void ThreadCache::free_allocation(const alaska::Mapping &m) {
    this->free_rate.track(1);
    void *ptr = m.get_pointer();

#ifdef USE_MALLOC
    ::free(ptr);
#else
    // Grab the page from the global heap (walk the page table).
    auto *page = this->runtime.heap.pt.get_unaligned(ptr);
    size_t size = page->size_of(ptr);
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
#endif
  }




  SizedPage *ThreadCache::new_sized_page(int cls) {
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

  void *ThreadCache::halloc(size_t size, bool zero) {
    if (unlikely(size == 0)) return NULL;

    if (unlikely(alaska::should_be_huge_object(size))) {
      log_debug("ThreadCache::halloc huge size=%zu\n", size);
      // Allocate the huge allocation.
      return this->runtime.heap.huge_allocator.allocate(size);
    }



    log_info("ThreadCache::halloc size=%zu", size);
    alaska::AllocationRequest req(*this, size);
    req.zero = zero;

    int cls = alaska::size_to_class(size);
    this->allocation_rate.track(1);
    void *ptr;
    SizedPage *page = size_classes[cls];
    if (unlikely(page == nullptr)) page = new_sized_page(cls);

    ptr = page->allocate_handle(req);
    if (unlikely(ptr == nullptr)) {
      // OOM?
      page = new_sized_page(cls);
      ptr = page->allocate_handle(req);
      ALASKA_ASSERT(ptr != nullptr, "OOM!");
    }

    return ptr;
  }


  void *ThreadCache::hrealloc(void *handle, size_t new_size) {
    // TODO: There is a race here... I think its okay, as a realloc really should
    // be treated like a UAF, and ideally another thread would not access the handle
    // while it is being reallocated.
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);

    auto original_size = this->get_size(handle);
    void *new_handle = this->halloc(new_size);


    // We should copy the minimum of the two sizes between the allocations.
    size_t copy_size = original_size > new_size ? new_size : original_size;

    handle_memcpy(new_handle, handle, copy_size);

    hfree(handle);

    return new_handle;
  }


  void ThreadCache::hfree(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (unlikely(m == nullptr)) {
      bool worked = this->runtime.heap.huge_allocator.free(handle);
      (void)worked;
      // ALASKA_ASSERT(worked, "huge free failed");
      return;
    }
    // Free the allocation behind a mapping
    free_allocation(*m);
    m->set_pointer(nullptr);
    // Return the handle to the handle table.
    this->runtime.handle_table.put(m, this);
  }


  size_t ThreadCache::get_size(void *handle) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(handle);
    if (m == nullptr) {
      return this->runtime.heap.huge_allocator.size_of(handle);
    }

    if (m->is_free()) return 0;
    void *ptr = m->get_pointer();
#ifdef USE_MALLOC
    return ::malloc_usable_size(ptr);
#else
    auto *page = this->runtime.heap.pt.get_unaligned(ptr);
    if (page == nullptr) return this->runtime.heap.huge_allocator.size_of(ptr);
    return page->size_of(ptr);
#endif
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

  void ThreadCache::free_mapping(alaska::Mapping *m) { this->runtime.handle_table.put(m, this); }


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
