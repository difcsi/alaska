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
#include <alaska/SizeClass.hpp>
#include <alaska/ShardedFreeList.hpp>
#include <alaska/SizedAllocator.hpp>

namespace alaska {
  struct Block;

  // A SizedPage allocates objects of one specific size class.
  class SizedPage final : public alaska::HeapPage {
   public:
    using HeapPage::HeapPage;
    ~SizedPage(void) override;


    void *alloc(const alaska::Mapping &m, alaska::AlignedSize size) override;
    bool release_local(const alaska::Mapping &m, void *ptr) override;
    bool release_remote(const alaska::Mapping &m, void *ptr) override;
    void set_size_class(int cls);
    int get_size_class(void) const { return size_class; }
    size_t get_object_size(void) const { return object_size; }


    // Compact the page.
    long compact(void);
    // Run through the page and validate as much info as possible w/ asserts.
    void validate(void);

    long object_capacity(void) const { return this->capacity; }



    float fragmentation(void) override {
      return (float)num_free_in_free_list() / (float)object_extent();
    }

    // How many free slots are there? (We return an estimate!)
    inline long available(void) {
      // TODO:
      return 0;
    }
    auto get_rates(TimeCache &tc) {
      // TODO:
      return AllocationRate();
    }


    inline void dump_live(void) {
      for (int i = 0; i < capacity; i++) {
        ObjectHeader *h = ind_to_header(i);
        if (is_allocated(h)) {
          printf("Object %d: %p, handle_id: %lu is live.\n", i, h, h->handle_id);
        }
      }
    }


    inline long num_free(void) const {
      return num_free_in_free_list() + num_free_in_bump_allocator();
    }

    inline long num_free_in_free_list(void) const { return freelist.num_free(); }

    inline long num_free_in_bump_allocator(void) const {
      return (((uintptr_t)objects_end - (uintptr_t)bump_next) / object_size);
    }

   private:
    struct SizePageBlock {
      ObjectHeader header;
      SizePageBlock *next;

      inline void setNext(SizePageBlock *n) { next = n; }
      inline SizePageBlock *getNext(void) const { return next; }
      inline void markFreed(void) {}
      inline void markAllocated(void) {}
    };
    ShardedFreeList<SizePageBlock> freelist;


    long extend(long count);

    void *alloc_slow(const alaska::Mapping &m, alaska::AlignedSize size);

    inline void release_local(ObjectHeader *h) {
      freelist.free_local(h);
      h->handle_id = 0;
    }

    inline void release_remote(ObjectHeader *h) {
      freelist.free_remote(h);
      h->handle_id = 0;
    }

    inline bool is_allocated(ObjectHeader *h) { return h->handle_id != 0; }

    // An internal-ish number which represents the extent of objects
    // which have been bump allocated in this allocator so far.
    inline long object_extent(void) {
      return ((off_t)bump_next - (off_t)objects_start) / object_size;
    }
    ////////////////////////////////////////////////

    int size_class;      // The size class of this page
    size_t object_size;  // The byte size of the size class of this page (saves a load)
    long capacity;       // how many objects + headers fit into this page



    void *objects_start;  // The start of the object memory
    void *objects_end;    // The end of the object memory (exclusive)
    void *bump_next;      // The next object to be bump allocated.

    // SizedAllocator allocator;

    ObjectHeader *ind_to_header(long ind) {
      size_t real_size = object_size + sizeof(ObjectHeader);
      return (ObjectHeader *)((char *)this->memory + (ind * real_size));
    }

    long header_to_ind(ObjectHeader *h) {
      size_t real_size = object_size + sizeof(ObjectHeader);
      return ((char *)h - (char *)this->memory) / real_size;
    }
  };



}  // namespace alaska
