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

#include <alaska/HeapPage.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/ShardedFreeList.hpp>
#include <alaska/SizedAllocator.hpp>
#include "alaska/AllocationRequest.hpp"

namespace alaska {

  // A conjoined page is a heap page which stores multiple logical
  // objects in a single handle, conjoined together contiguously. The
  // main goal of this is to reduce the number of unique cache entries
  // required to use handles for objects that are on average smaller
  // than a cache line. The thinking is that if you have, say, four
  // heap objects mapped into a single handle, translating them
  // requires 1/4th the cache space compared to if they each had their
  // own handles.
  class ConjoinedPage : public alaska::HeapPage {
   public:
    using HeapPage::HeapPage;
    ~ConjoinedPage(void) override;


    void *allocate_handle(const AllocationRequest &req) override;
    bool release_local(const Mapping& m, void* ptr) override;
    bool release_remote(const Mapping& m, void* ptr) override;
    size_t size_of(void* ptr) override;
  };
}  // namespace alaska
