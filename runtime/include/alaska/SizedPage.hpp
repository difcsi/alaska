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
  class SizedPage : public alaska::HeapPage {
   public:
    using HeapPage::HeapPage;
    ~SizedPage(void) override;


    void *alloc(const alaska::Mapping &m, alaska::AlignedSize size) override;
    bool release_local(const alaska::Mapping &m, void *ptr) override;
    bool release_remote(const alaska::Mapping &m, void *ptr) override;

    // How many free slots are there? (We return an estimate!)
    inline long available(void) { return this->allocator.num_free(); }
    void set_size_class(int cls);
    int get_size_class(void) const { return size_class; }
    size_t get_object_size(void) const { return object_size; }


    // Compact the page.
    long compact(void);
    // Run through the page and validate as much info as possible w/ asserts.
    void validate(void);
    // Move every object somewhere else in the page (for testing)
    long jumble(void);

    long object_capacity(void) const { return this->capacity; }
    auto get_rates(TimeCache &tc) { return allocator.get_rates(tc); }

   private:
    ////////////////////////////////////////////////

    int size_class;      // The size class of this page
    size_t object_size;  // The byte size of the size class of this page (saves a load)
    long capacity;       // how many objects + headers fit into this page
    long live_objects;   // how many live objects are allocated

    SizedAllocator allocator;

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
