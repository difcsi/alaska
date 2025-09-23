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

#include <ck/vec.h>
#include <stdlib.h>
#include <stdint.h>
#include <alaska/alaska.hpp>
#include <alaska/Logger.hpp>
#include <alaska/HeapPage.hpp>
#include <alaska/SizedAllocator.hpp>

namespace alaska {

  // A locality page is meant to strictly bump allocate objects of variable size in order
  // of their expected access pattern in the future. It's optimized for moving objects into
  // this page, and the expected lifetimes of these objects is long enough that we don't really
  // care about freeing or re-using the memory occupied by them when they are gone.
  class LocalityPage final : public alaska::HeapPage {
   public:
    LocalityPage(void *backing_memory)
        : alaska::HeapPage(backing_memory) {
      snprintf(this->name, sizeof(this->name), "Locality");
      bump_next = (void *)this->memory_start();
    }

    // Available - The memory which can be allocated
    size_t available(void) { return memory_end() - (uintptr_t)bump_next; }
    // Committed - The memory which has been allocated.
    size_t committed(void) { return (uintptr_t)bump_next - memory_start(); }

    ~LocalityPage() override;

    // the main interface
    void *alloc(const alaska::Mapping &m, alaska::AlignedSize size) override;
    bool release_local(const alaska::Mapping &m, void *ptr) override;


    // Fragmentation - How much of the committed memory was freed?
    float fragmentation(void) override { return (float)freed_bytes / (float)committed(); }

   private:
    size_t freed_bytes = 0;
    void *bump_next;
  };
};  // namespace alaska
