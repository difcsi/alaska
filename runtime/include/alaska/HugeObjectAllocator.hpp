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

#include <stdlib.h>
#include <alaska/list_head.h>
#include <ck/lock.h>

namespace alaska {


  // The HugeObjectAllocator can be configured with different strategies. The main one
  // uses a custom mmap based strategy to track large objects. The other option is one
  // which is backed by libc's malloc, instead
  enum class HugeAllocationStrategy {
    CUSTOM_MMAP_BACKED,
    MALLOC_BACKED,
  };
  class HugeObjectAllocator final {
   public:
    HugeObjectAllocator(HugeAllocationStrategy strat);
    ~HugeObjectAllocator();

    void* allocate(size_t size);
    // Free a huge allocation. Returns false if it was not managed by this heap.
    bool free(void* ptr);
    // Get the size of a huge allocation. Returns 0 if it was not managed by this heap.
    size_t size_of(void* ptr);

    // Check if the allocator owns a pointer
    // NOTE: this walk the `allocations` list!
    bool owns(void* ptr);

    // ---- liballocs integration (raw backing-pointer queries) ----------------
    // These mirror the sized-heap helpers in liballocs_export.cpp so liballocs can
    // resolve huge objects the same way it resolves sized objects. They accept an
    // *interior* backing pointer and walk the `allocations` list. All three return
    // the "not ours" sentinel for the MALLOC_BACKED strategy (those objects are
    // already indexed by liballocs' own malloc allocator -- there is no HugeHeader
    // to resolve interior pointers against).

    // Start of the huge object containing `interior`, or null if not ours.
    void* object_base(void* interior);
    // Size liballocs should use for `interior`'s object: the caller's requested
    // size PLUS the trailing-insert reserve, so liballocs' insert_for_chunk lands
    // on the reserved tail (at base + requested_size) rather than on user data --
    // exactly like the sized heap reports its slot size. 0 if not ours.
    size_t backing_size_of(void* interior);
    // Extent [base, base+size) of the whole mmap region backing `interior`, for
    // liballocs to claim as a bigalloc. Returns true and fills the outputs on hit.
    bool extent_of(void* interior, void** out_base, size_t* out_size);

   private:
    HugeAllocationStrategy strat;
    ck::mutex m_lock;
    struct list_head allocations = LIST_HEAD_INIT(allocations);




    struct alignas(16) HugeHeader {
      struct list_head list;
      size_t mapping_size;     // the size of the mmap region
      size_t allocation_size;  // the size of the allocation.
      void* data() { return (void*)((uintptr_t)this + sizeof(HugeHeader)); }
      static HugeHeader* from_data(void* data) {
        return (HugeHeader*)((uintptr_t)data - sizeof(HugeHeader));
      }
    };

    HugeHeader* find_header(void* ptr);
    // Like find_header, but matches an *interior* pointer against each object's
    // [data, data+allocation_size) range (not just an exact base match).
    HugeHeader* find_header_containing(void* ptr);
  };
}  // namespace alaska
