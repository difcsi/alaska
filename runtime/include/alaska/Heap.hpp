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

#include <alaska/ObjectHeader.hpp>
#include <alaska/HeapPage.hpp>
#include <alaska/SizedPage.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/Magazine.hpp>
#include <alaska/HugeObjectAllocator.hpp>
#include <alaska/track.hpp>
#include "alaska/Configuration.hpp"
#include "alaska/LocalityPage.hpp"
#include <ck/vec.h>
#include <stdlib.h>
#include <ck/lock.h>

namespace alaska {
  static constexpr size_t kilobyte = 1024;
  static constexpr size_t megabyte = 1024 * kilobyte;
  static constexpr size_t gigabyte = 1024 * megabyte;


  // For now, the heap is a fixed size, large contiguious
  // block of memory managed by the PageManager. Eventually,
  // we will split it up into different mmap regions, but that's
  // just a problem solved by another layer of allocators.


#ifndef HEAP_SIZE_SHIFT_FACTOR
#define HEAP_SIZE_SHIFT_FACTOR 35
#endif
#ifdef ALASKA_TRACK_VALGRIND
  static constexpr uint64_t heap_size_shift_factor = 33;
#else
  static constexpr uint64_t heap_size_shift_factor = HEAP_SIZE_SHIFT_FACTOR;
#endif

  static constexpr size_t heap_size = 1LU << heap_size_shift_factor;



  // The PageManager is responsible for managing the memory backing the heap, and subdividing it
  // into pages which are handed to HeapPage instances. The main interface here is to allocate a
  // page of size alaska::page_size, and allow it to be freed again. Fundamentally, the PageManager
  // is a trivial bump allocator that uses a free-list to manage reuse.
  //
  // The methods behind this structure are currently behind a lock.
  class PageManager final {
   public:
    PageManager();
    ~PageManager();


    void *alloc_page(void);
    void free_page(void *page);
    void *get_start(void) const { return heap; }

    inline bool contains(void *ptr) { return (ptr >= heap && ptr < end); }


    double get_usage_frac(void) const {
      return 100.0 * (alloc_count / (double)(heap_size / page_size));
    }

    inline void *get_page(off_t i) { return (void *)((off_t)heap + (i * page_size)); }
    inline uint64_t get_allocated_page_count(void) const { return alloc_count; }

   private:
    struct FreePage {
      FreePage *next;
    };

    // This is the memory backing the heap. It is `alaska::heap_size` bytes long.
    void *heap;
    void *end;   // the end of the heap. If bump == end, we are OOM. make heap_size bigger!
    void *bump;  // the current bump pointer
    uint64_t alloc_count = 0;  // How many pages are currently in use
    ck::mutex lock;            // Just a lock.

    alaska::PageManager::FreePage *free_list;
  };



  // allocate pages to fit `bytes` bytes from the kernel.
  void *mmap_alloc(size_t bytes);
  // free pages allocated by mmap_alloc
  void mmap_free(void *ptr, size_t bytes);

  // TODO: THREAD SAFETY! THREAD SAFETY! THREAD SAFETY!
  class HeapPageTable {
   public:
    HeapPageTable(void *heap_start);
    ~HeapPageTable(void);
    alaska::HeapPage *get(void *page);            // Get the HeapPage given an aligned address
    alaska::HeapPage *get_unaligned(void *page);  // Get the HeapPage given an unaligned address
    void set(void *page, alaska::HeapPage *heap_page);


    const ck::vec<alaska::HeapPage *> &get_table(void) const { return table; }

   private:
    // A pointer to the start of the heap. This is used to compute the page number in the heap.
    void *heap_start;
    alaska::HeapPage **walk(void *page, bool ensure = false);

    ck::mutex lock;  // TODO: reader/writer lock!
    ck::vec<alaska::HeapPage *> table;
  };



  inline alaska::HeapPage *HeapPageTable::get(void *page) {
    auto *p = walk(page, false);
    if (p == nullptr) return nullptr;
    return *p;
  }


  inline alaska::HeapPage *HeapPageTable::get_unaligned(void *addr) {
    HeapPageHeader *h = (HeapPageHeader *)((uintptr_t)addr & ~(alaska::page_size - 1));
    return h->owner;
  }

  inline void HeapPageTable::set(void *page, alaska::HeapPage *hp) { *walk(page, true) = hp; }


  inline alaska::HeapPage **HeapPageTable::walk(void *vpage, bool ensure) {
    // ck::scoped_lock lk(this->lock);  // TODO: reader/writer lock

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

    return &table[page_number];
  }


  class Heap final {
   public:
    PageManager pm;
    HeapPageTable pt;


    Heap(alaska::Configuration &config);
    ~Heap(void);

    // Get an unowned sized page given a certain size request.
    // TODO: Allow filtering by fullness?
    alaska::SizedPage *get_sizedpage(size_t size, ThreadCache *owner = nullptr);
    alaska::LocalityPage *get_localitypage(size_t size_requirement, ThreadCache *owner = nullptr);


    void put_page(alaska::SizedPage *page);
    void put_page(alaska::LocalityPage *page);



    // Run a "heap collection" phase. This basically just means
    // walking over the HeapPage instances, collecting statistics and
    // updating datastructures. This will take the lock, so it
    // currently only makes sense calling from a single thread.
    void collect(void);


    // Dump the state of the global heap to some file stream.
    void dump(FILE *stream);
    void dump_html(FILE *stream);
    void dump_json(FILE *stream);

    // Run a compaction on sized pages.
    long compact_sizedpages(void);
    long compact_locality_pages(void);

    inline bool contains(void *ptr) { return this->pm.contains(ptr); }


    template <typename Fn>
    void for_each_page(Fn fn) {
      for (size_t i = 0; i < alaska::num_size_classes; i++) {
        size_classes[i].for_each([=](auto *p) {
          fn((alaska::HeapPage *)p);
          return true;
        });
      }
      locality_pages.for_each([=](auto *p) {
        fn((alaska::HeapPage *)p);
        return true;
      });
    }


   private:
    template <typename T, typename Fn>
    T *find_or_alloc_page(
        alaska::Magazine<T> &mag, ThreadCache *owner, size_t avail_requirement, Fn &&init);

    // This lock is taken whenever global state in the heap is changed by a thread cache.
    ck::mutex lock;
    alaska::Magazine<alaska::SizedPage> size_classes[alaska::num_size_classes];
    alaska::Magazine<alaska::LocalityPage> locality_pages;
  };



  template <typename T, typename Fn>
  T *Heap::find_or_alloc_page(
      alaska::Magazine<T> &mag, ThreadCache *owner, size_t avail_requirement, Fn &&init_fn) {
    if (mag.size() != 0) {
      T *best = nullptr;
      // alaska::printf("Searching for page with at least %zu available\n", avail_requirement);
      mag.for_each([&](T *p) {
        size_t avail = p->available();
        if ((size_t)avail >= (size_t)avail_requirement and p->get_owner() == nullptr) {
          // best = p;
          if (best == nullptr) {
            best = p;
            return true;
          }


          if ((long)avail >= (long)best->available()) {
            best = p;
          } else {
            // .. Nothing
          }
        }
        return true;
      });

      if (best != NULL) {
        best->set_owner(owner);
        return best;
      }
    }


    // Allocate a new sized page
    void *memory = this->pm.alloc_page();
    T *p = new T(memory);
    // Map it in the page table for fast lookup
    pt.set(memory, p);
    mag.add(p);
    p->set_owner(owner);

    init_fn(p);

    return p;
  }
}  // namespace alaska
