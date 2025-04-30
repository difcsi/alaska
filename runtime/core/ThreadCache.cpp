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

namespace alaska {

  ThreadCache::ThreadCache(int id, alaska::Runtime &rt)
      : id(id)
      , runtime(rt)
      , localizer(rt.config, *this) {
    handle_table_churn++;
    handle_slab = runtime.handle_table.new_slab(this);
  }


  void *ThreadCache::allocate_backing_data(const alaska::Mapping &m, size_t size) {
    int cls = alaska::size_to_class(size);
    this->allocation_rate.track(1);
    void *ptr;

    SizedPage *page = size_classes[cls];
    if (unlikely(page == nullptr)) {
      page = new_sized_page(cls);
    }
    ptr = page->alloc(m, size);
    if (unlikely(ptr == nullptr)) {
      // OOM?
      page = new_sized_page(cls);
      ptr = page->alloc(m, size);
      ALASKA_ASSERT(ptr != nullptr, "OOM!");
    }

    return ptr;
  }




  void ThreadCache::free_allocation(const alaska::Mapping &m) {
    this->free_rate.track(1);
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
    auto header = alaska::ObjectHeader::from(ptr);
    return header->object_size();
  }

  Mapping *ThreadCache::new_mapping(void) {
    auto m = handle_slab->alloc();

    if (unlikely(m == NULL)) {
      handle_table_churn++;  // record that we are looking for a new handle table slab.
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


  long ThreadCache::localize(alaska::Mapping *m, long allowed_depth) {
    long moved_count = 0;
    void *ptr = m->get_pointer();
    if (ptr == nullptr || m->is_free() || m->is_pinned()) return 0;
    auto *source_page = this->runtime.heap.pt.get_unaligned(ptr);

    // Validate that we can indeed move this object from the page.
    if (unlikely(source_page == nullptr)) {
      return 0;
    }

    auto header = alaska::ObjectHeader::from(ptr);

    // Ask the page for the size of the pointer
    auto size = header->object_size();
    // If the size is too large, don't bother
    if (size > alaska::locality_slab_size / 2) return 0;


    // This size comparison is kinda nonsense
    if (locality_page == nullptr or
        locality_page->available() < size + sizeof(alaska::ObjectHeader) + 8) {
      locality_page = new_locality_page(size + 32);
    }

    void *new_location = nullptr;
    while (true) {
      new_location = locality_page->alloc(*m, size);
      if (new_location != nullptr) break;
      locality_page = new_locality_page(size + 32);
    }

    hotness_hist[header->hotness]--;
    // Now for the actual localization
    memcpy(new_location, ptr, size);       // Copy the data
    source_page->release_remote(*m, ptr);  // Release the old location
    m->set_pointer(new_location);          // Write the new location
    moved_count += 1;

    // TODO: localize one level of pointers?
    if (allowed_depth > 0) {
      auto *header = alaska::ObjectHeader::from(new_location);
      auto *ptr = (void **)new_location;
      size_t scan_size = 128;
      if (size < scan_size) scan_size = size;
      auto *end = (void **)((char *)ptr + scan_size);
      while (ptr < end) {
        auto *p = *ptr;
        if (p != nullptr) {
          auto *m = alaska::Mapping::from_handle_safe(p);
          if (m != nullptr) {
            moved_count += localize(m, allowed_depth - 1);
          }
        }
        ptr++;
      }
    }

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
