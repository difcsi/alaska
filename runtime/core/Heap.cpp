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


#include <sys/mman.h>
#include <alaska/Logger.hpp>
#include <alaska/Heap.hpp>
#include "alaska/HeapPage.hpp"
#include "alaska/HugeObjectAllocator.hpp"
#include "alaska/LocalityPage.hpp"
#include "alaska/SizeClass.hpp"
#include "alaska/utils.h"
#include <alaska/ThreadCache.hpp>


namespace alaska {


  PageManager::PageManager(void) {
    auto prot = PROT_READ | PROT_WRITE;
    auto flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    this->heap = mmap(NULL, alaska::heap_size, prot, flags, -1, 0);
    ALASKA_ASSERT(
        this->heap != MAP_FAILED, "Failed to allocate the heap's backing memory. Aborting.");

    // Set the bump allocator to the start of the heap.
    this->bump = this->heap;
    // round bump up to 2mb
    this->bump =
        (void *)(((uintptr_t)this->bump + alaska::page_size - 1) & ~(alaska::page_size - 1));
    this->end = (void *)((uintptr_t)this->heap + alaska::heap_size);

    // Initialize the free list w/ null, so the first allocation is a simple bump.
    this->free_list = nullptr;

    log_debug("PageManager: Heap allocated at %p", this->heap);
  }

  PageManager::~PageManager() {
    log_debug("PageManager: Dellocating heap at %p", this->heap);
    munmap(this->heap, alaska::heap_size);
  }


  void *PageManager::alloc_page(void) {
    ck::scoped_lock lk(this->lock);  // TODO: don't lock.


    if (this->free_list != nullptr) {
      // If we have a free page, pop it off the free list and return it.
      FreePage *fp = this->free_list;
      this->free_list = fp->next;
      log_trace("PageManager: reusing free page at %p", fp);
      alloc_count++;
      return (void *)fp;
    }

    // If we don't have a free page, we need to allocate a new one with the bump allocator.
    void *page = this->bump;
    this->bump = (void *)((uintptr_t)this->bump + alaska::page_size);

    log_trace("PageManager: end = %p", this->end);

    // TODO: this is *so unlikely* to happen. This check is likely expensive and not needed.
    ALASKA_ASSERT(page < this->end, "Out of memory in the page manager.");
    alloc_count++;

    return page;
  }

  void PageManager::free_page(void *page) {
    // check that the pointer is within the heap and early return if it is not
    if (unlikely(page < this->heap || page >= this->end)) {
      return;
    }

    ck::scoped_lock lk(this->lock);  // TODO: don't lock.

    // cast the page to a FreePage to store metadata in.
    FreePage *fp = (FreePage *)page;

    // Super simple: push to the free list.
    fp->next = this->free_list;
    this->free_list = fp;

    alloc_count--;
  }




  ////////////////////////////////////

  HeapPageTable::HeapPageTable(void *heap_start)
      : heap_start(heap_start) {}

  HeapPageTable::~HeapPageTable() {}




  ////////////////////////////////////
  Heap::Heap(alaska::Configuration &config)
      : pm()
      , pt(pm.get_start()) {
    log_debug("Heap: Initialized heap");
  }

  Heap::~Heap(void) {}

  SizedPage *Heap::get_sizedpage(size_t size, ThreadCache *owner) {
    ck::scoped_lock lk(this->lock);  // TODO: don't lock.
    int cls = alaska::size_to_class(size);
    auto &mag = this->size_classes[cls];
    // Look for a sized page in the magazine with at least one allocation space available.
    // TODO: it would be smart to adjust this requirement dynamically based on the allocation
    // request.
    auto *p = this->find_or_alloc_page<SizedPage>(mag, owner, 1, [=](auto p) {
      // alaska::printf("Allocating sized page for class %d (%zu bytes)\n", cls, size);
      p->set_size_class(cls);
    });
    return p;
  }


  LocalityPage *Heap::get_localitypage(size_t size_requirement, ThreadCache *owner) {
    ck::scoped_lock lk(this->lock);  // TODO: don't lock.
    auto *p = this->find_or_alloc_page<LocalityPage>(
        locality_pages, owner, size_requirement, [](auto *p) {
        });
    return p;
  }


  void Heap::put_page(SizedPage *page) {
    // Return a SizedPage back to the global (unowned) heap.
    ck::scoped_lock lk(this->lock);  // TODO: don't lock.
    page->set_owner(nullptr);
  }


  void Heap::put_page(LocalityPage *page) {
    // Return a SizedPage back to the global (unowned) heap.
    ck::scoped_lock lk(this->lock);  // TODO: don't lock.
    page->set_owner(nullptr);
  }


  void Heap::dump(FILE *stream) {
    long i = 0;

    size_t total_committed = 0;

    for (auto &mag : size_classes) {
      if (mag.size() == 0) continue;
      auto size = alaska::class_to_size(i);

      fprintf(stream, "SizePages<%zu>\n", size);
      i += 1;

      long page_index = 0;
      mag.foreach ([&](SizedPage *p) {
        size_t committed = p->committed();
        total_committed += committed;
        fprintf(stream, "  - %p avail:%zu  commit:%zu  frag:%f\n", p, p->available(), committed,
            p->fragmentation());
        return true;
      });
    }


    fprintf(stream, "LocalityPages\n");
    locality_pages.foreach ([&](LocalityPage *p) {
      size_t committed = p->committed();
      total_committed += committed;
      fprintf(stream, "  - %p avail:%zu  commit:%zu  frag:%f\n", p, p->available(), committed,
          p->fragmentation());
      return true;
    });

    fprintf(stream, "Total committed memory: %zu bytes (%fmb)\n", total_committed,
        total_committed / (1024.0 * 1024.0));

    fprintf(stream, "\n");
  }



  void Heap::dump_html(FILE *stream) {
    auto dump_page = [&](auto page) {
      if (page == NULL) return true;
      fprintf(stream, "<tr>");
      fprintf(stream, "<td>%p</td>", page);
      fprintf(stream, "<td>");
      page->dump_html(stream);
      fprintf(stream, "</tr>\n");
      return true;
    };

    locality_pages.foreach (dump_page);
    // for (auto &mag : size_classes)
    //   mag.foreach (dump_page);
  }


  void Heap::dump_json(FILE *stream) {
    fprintf(stream, "{\"pages\": [");
    for (off_t i = 0; true; i++) {
      auto page = pt.get(pm.get_page(i));
      if (page == NULL) break;
      if (i != 0) fprintf(stream, ",");
      page->dump_json(stream);
    }
    fprintf(stream, "]}");
  }

  void Heap::collect() {
    ck::scoped_lock lk(this->lock);


    // TODO:
  }


  long Heap::compact_sizedpages(void) {
    size_t total_bytes = 0;
    size_t zero_bytes = 0;
    size_t total_objects = 0;


    size_t bytes_saved = 0;
    long c = 0;
    for (auto &mag : size_classes) {
      mag.foreach ([&](SizedPage *sp) {
        long z = 0, t = 0;
        if (true || sp->fragmentation() > 0.15) {
          zero_bytes += z;
          total_bytes += t;
          total_objects += t / sp->get_object_size();

          long moved = sp->compact();
          c += moved;
          bytes_saved += moved * sp->get_object_size();
        }
        return true;
      });
    }
    return bytes_saved;
  }

  long Heap::compact_locality_pages(void) {
    return 0;
    long c = 0;
    alaska::printf("Locality pages: %lu\n", locality_pages.size());
    locality_pages.foreach ([&](LocalityPage *lp) {
      alaska::printf(" - Locality page %p  commit:%zu, avail:%zu, frag:%.2f%%\n", lp,
          lp->committed(), lp->available(), lp->fragmentation() * 100);
      return true;
    });
    return c;
  }



  void *mmap_alloc(size_t bytes) {
    auto prot = PROT_READ | PROT_WRITE;
    auto flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    // round bytes up to 4096
    bytes = (bytes + 4095) & ~4095;
    void *ptr = mmap(NULL, bytes, prot, flags, -1, 0);
    ALASKA_ASSERT(ptr != MAP_FAILED, "Failed to allocate memory with mmap.");
    return ptr;
  }

  void mmap_free(void *ptr, size_t bytes) {
    // round bytes up to 4096
    bytes = (bytes + 4095) & ~4095;
    munmap(ptr, bytes);
  }



}  // namespace alaska
