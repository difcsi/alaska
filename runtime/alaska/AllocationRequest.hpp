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
#include <stdint.h>
#include <stdlib.h>

namespace alaska {
  class ThreadCache;
  class Domain;


  // This is a simple structure which collects all the parameters needed
  // to request an allocation from a HeapPage.
  struct AllocationRequest {
    AllocationRequest(ThreadCache &requestor, Domain &domain, size_t sz)
        : requestor(requestor)
        , domain(domain)
        , size(sz) {}

    ThreadCache &requestor;  // The thread cache requesting this allocation
    Domain &domain;          // The domain for handle slab allocation
    size_t size;             // The number of bytes (at least) being requested.
    bool zero = false;       // Should the data be zeroed?
  };
};  // namespace alaska
