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
    this->end = (void *)((uintptr_t)this->heap + alaska::heap_size);

    // Initialize the free list w/ null, so the first allocation is a simple bump.
    this->free_list = nullptr;

    log_debug("PageManager: Heap allocated at %p", this->heap);
  }

  PageManager::~PageManager() {
    log_debug("PageManager: Deallocating heap at %p", this->heap);
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
    log_trace("PageManager: bumping to %p", this->bump);
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


  alaska::HeapPage *HeapPageTable::get(void *page) {
    auto *p = walk(page, false);
    if (p == nullptr) return nullptr;
    return *p;
  }


  alaska::HeapPage *HeapPageTable::get_unaligned(void *addr) {
    uintptr_t heap_offset = (uintptr_t)addr - (uintptr_t)heap_start;
    uintptr_t page_ind = heap_offset / alaska::page_size;
    void *page = (void *)((uintptr_t)heap_start + page_ind * alaska::page_size);

    auto *p = walk(page, false);
    if (p == nullptr) return nullptr;
    return *p;
  }

  void HeapPageTable::set(void *page, alaska::HeapPage *hp) { *walk(page, true) = hp; }


  alaska::HeapPage **HeapPageTable::walk(void *vpage, bool ensure) {
    ck::scoped_lock lk(this->lock);  // TODO: reader/writer lock

    // `page` here means the offset from the start of the heap.
    uintptr_t page_off = (uintptr_t)vpage - (uintptr_t)heap_start;
    // Extrac the page number (just an index into the page table structure)
    off_t page_number = page_off >> alaska::page_shift_factor;
    // printf("vpage: %p, heap_start: %p, page_off: %lu, page_number: %lu\n", vpage, heap_start,
    //     page_off, page_number);


    if (page_number >= table.size()) {
      if (!ensure) {
        return nullptr;
      }
      // If the page number is greater than the size of the table, we need to grow the table.
      table.resize(page_number + 1);
    }

    ALASKA_ASSERT(page_number < table.size(), "Page number out of bounds.");
    return &table[page_number];
  }




  ////////////////////////////////////
  Heap::Heap(alaska::Configuration &config)
      : pm()
      , pt(pm.get_start())
      , huge_allocator(config.huge_strategy) {
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
    auto *p = this->find_or_alloc_page<SizedPage>(mag, owner, 1, [&](auto p) {
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

    alaska::TimeCache timecache;
    for (auto &mag : size_classes) {
      if (mag.size() == 0) continue;
      auto size = alaska::class_to_size(i);

      fprintf(stream, "%8zu,%-3zu: ", size, mag.size());
      i += 1;

      long page_index = 0;
      mag.foreach ([&](SizedPage *sp) {
        // if (page_index == 0) printf("(%8lu) ", sp->object_capacity());
        bool color = sp->get_owner() != nullptr;
        long avail = sp->available();
        float avail_frac = avail / (float)sp->object_capacity();
        float used = sp->object_capacity() - avail;
        float used_frac = 1.0 - avail_frac;

        // if (sp->get_owner() != nullptr) {
        //   fprintf(stream, "\033[48;2;0;0;%dm", (int)(255 * used_frac));
        // } else {
        //   fprintf(stream, "\033[48;2;%d;0;0m", (int)(255 * used_frac));
        // }

        auto r = sp->get_rates(timecache);
        auto pressure = r.pressure();

        auto tend = r.tendancy();
        auto tend_norm = (tend + 1.0) / 2.0;
        int blue = (255 * tend_norm);
        int red = 255 - blue;

        fprintf(stream, "\033[48;2;%d;0;%dm", red, blue);

        fprintf(stream, "%7.2f%% ", 100.0 * used_frac);
        // fprintf(stream, "%8.2f/%7.2fkb ", avail * size / 1024.0, alaska::page_size / 1024.0);
        // fprintf(stream, "%8.2fkb ", avail * size / 1024.0);
        // fprintf(stream, "%8.2f/%7.2fkb ", avail * size / 1024.0, alaska::page_size / 1024.0);
        // fprintf(stream, "%8.2fkb/%7.2fkb ", used * size / 1024.0, alaska::page_size / 1024.0);
        // fprintf(stream, "%.3fu ", used_frac);
        // fprintf(stream, "%+12.2fp ", pressure);
        // fprintf(stream, "%+5.2ft ", tend);
        // fprintf(stream, "%+12.0fΔ ", r.delta());
        // fprintf(stream, "%+12.2fΔ ", r.allocation_bytes_per_second - r.free_bytes_per_second);

        fprintf(stream, "\e[0m ");

        page_index++;
        return true;
      });
      fprintf(stream, "\n");
    }
    // fprintf(stream, "\n");
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

    // A histogram for each byte value
    long byte_histogram[256] = {0};


    size_t bytes_saved = 0;
    long c = 0;
    for (auto &mag : size_classes) {
      mag.foreach ([&](SizedPage *sp) {
        long z = 0, t = 0;

        zero_bytes += z;
        total_bytes += t;
        total_objects += t / sp->get_object_size();

        long moved = sp->compact();
        c += moved;
        bytes_saved += moved * sp->get_object_size();
        return true;
      });
    }
    return bytes_saved;
  }

  long Heap::compact_locality_pages(void) {
    long c = 0;
    // printf("Utilizations:\n");
    long total_wasted = 0;
    long total_time = 0;
    locality_pages.foreach ([&](LocalityPage *lp) {
      int compaction_iterations = 0;
      auto start = alaska_timestamp();
      if (lp->utilization() < 0.8) {
        while (lp->compact() != 0) {
          compaction_iterations++;
        }
      }
      auto end = alaska_timestamp();
      total_time += (end - start);

      float u = lp->utilization();
      size_t wasted = lp->heap_size() * (1 - u);
      total_wasted += wasted;

      // printf("%p - %8f   waste: %5lukb   %4d iters in %9luns\n", lp, u, wasted / 1024,
      //     compaction_iterations, (end - start));
      return true;
    });
    // printf("Total wastage: %lukb\n", total_wasted / 1024);
    // printf("Tool %fms\n", total_time / 1000.0 / 1000.0);
    return c;
  }


  long Heap::jumble(void) {
    long c = 0;
    for (auto &mag : size_classes) {
      mag.foreach ([&](SizedPage *sp) {
        c += sp->jumble();
        return true;
      });
    }
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
