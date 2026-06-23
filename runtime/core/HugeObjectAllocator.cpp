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


#include <malloc.h>
#include <stdlib.h>

#include <alaska/Heap.hpp>
#include <alaska/HugeObjectAllocator.hpp>
#include <alaska/liballoc.h>
#include <alaska/list_head.h>

// Bytes reserved at the END of every huge object for a liballocs trailing
// `struct insert` (the per-object lifetime-policy mask). Same build-injected knob
// as the sized heap (see core/ThreadCache.cpp); 0 in vanilla Alaska, which makes
// every change below a no-op. Folded into the mmap size so the insert lives past
// the user's bytes, and reported by backing_size_of() so liballocs' insert_for_chunk
// lands on it. See contrib/liballocs/src/allocators/alaska.c.
#ifndef ALASKA_LIBALLOCS_INSERT_RESERVE
#define ALASKA_LIBALLOCS_INSERT_RESERVE 0
#endif

namespace alaska {
  HugeObjectAllocator::HugeObjectAllocator(HugeAllocationStrategy strat)
      : strat(strat) {
    INIT_LIST_HEAD(&this->allocations);
  }

  HugeObjectAllocator::~HugeObjectAllocator() {
    HugeHeader *entry, *temp;
    // Iterate over the list safely
    list_for_each_entry_safe(entry, temp, &this->allocations, list) {
      // Remove the entry from the list
      list_del(&entry->list);
      alaska::mmap_free((void*)entry, entry->mapping_size);
    }
  }



  void* HugeObjectAllocator::allocate(size_t size) {
    if (strat == HugeAllocationStrategy::MALLOC_BACKED) {
      return ::malloc(size);
    }
    ALASKA_ASSERT(strat == HugeAllocationStrategy::CUSTOM_MMAP_BACKED, "Invalid huge strat");

    ck::scoped_lock l(m_lock);

    // Reserve room past the user's bytes for liballocs' trailing insert (0 unless
    // the build injected a reserve). allocation_size stays the caller's exact
    // request -- only the mapping grows -- so size_of()/realloc semantics are
    // unchanged; backing_size_of() adds the reserve back for the insert math.
    size_t mapping_size =
        ((size + ALASKA_LIBALLOCS_INSERT_RESERVE + sizeof(HugeHeader)) + 4095) & ~4095;
    if (mapping_size < 4096) {
      mapping_size = 4096;
    }

    // Allocate memory using mmap
    void* memory = alaska::mmap_alloc(mapping_size);

    // Create a HugeHeader object to store metadata
    HugeHeader* header = reinterpret_cast<HugeHeader*>(memory);
    header->mapping_size = mapping_size;
    header->allocation_size = size;

    // Add the allocated memory to the list
    list_add(&header->list, &this->allocations);

    // Return a pointer to the user memory
    return header->data();
  }


  bool HugeObjectAllocator::free(void* ptr) {
    if (strat == HugeAllocationStrategy::MALLOC_BACKED) {
      ::free(ptr);
      return true;
    }
    ALASKA_ASSERT(strat == HugeAllocationStrategy::CUSTOM_MMAP_BACKED, "Invalid huge strat");
    ck::scoped_lock l(m_lock);

    // Get the HugeHeader object from the user pointer
    HugeHeader* header = find_header(ptr);
    if (header == nullptr) return false;

    // Remove the entry from the list
    list_del(&header->list);

    // Free the memory using mmap
    alaska::mmap_free((void*)header, header->mapping_size);
    return true;
  }


  size_t HugeObjectAllocator::size_of(void* ptr) {
    if (strat == HugeAllocationStrategy::MALLOC_BACKED) return ::malloc_usable_size(ptr);

    ck::scoped_lock l(m_lock);
    HugeHeader* header = find_header(ptr);
    if (header != nullptr) {
      return header->allocation_size;
    }
    return 0;
  }


  bool HugeObjectAllocator::owns(void* ptr) {
    if (strat == HugeAllocationStrategy::MALLOC_BACKED) return true;

    ck::scoped_lock l(m_lock);
    // If the header is null, this allocator doesn't own it.
    return find_header(ptr) != nullptr;
  }

  HugeObjectAllocator::HugeHeader* HugeObjectAllocator::find_header(void* ptr) {
    HugeHeader* entry;
    // Iterate over the list
    list_for_each_entry(entry, &this->allocations, list) {
      // Check if the user pointer matches the data pointer in the header
      if (entry->data() == ptr) {
        return entry;
      }
    }
    // If no matching header is found, return nullptr
    return nullptr;
  }

  HugeObjectAllocator::HugeHeader* HugeObjectAllocator::find_header_containing(void* ptr) {
    HugeHeader* entry;
    list_for_each_entry(entry, &this->allocations, list) {
      auto base = (uintptr_t)entry->data();
      // Match against the user range only (not the reserved insert tail), so an
      // interior pointer into the reserve is not reported as user data -- mirrors
      // the sized heap, whose object_base also covers only the requested bytes.
      if ((uintptr_t)ptr >= base && (uintptr_t)ptr < base + entry->allocation_size) {
        return entry;
      }
    }
    return nullptr;
  }

  void* HugeObjectAllocator::object_base(void* interior) {
    if (strat != HugeAllocationStrategy::CUSTOM_MMAP_BACKED) return nullptr;
    ck::scoped_lock l(m_lock);
    HugeHeader* header = find_header_containing(interior);
    return header ? header->data() : nullptr;
  }

  size_t HugeObjectAllocator::backing_size_of(void* interior) {
    if (strat != HugeAllocationStrategy::CUSTOM_MMAP_BACKED) return 0;
    ck::scoped_lock l(m_lock);
    HugeHeader* header = find_header_containing(interior);
    if (header == nullptr) return 0;
    return header->allocation_size + ALASKA_LIBALLOCS_INSERT_RESERVE;
  }

  bool HugeObjectAllocator::extent_of(void* interior, void** out_base, size_t* out_size) {
    if (strat != HugeAllocationStrategy::CUSTOM_MMAP_BACKED) return false;
    ck::scoped_lock l(m_lock);
    HugeHeader* header = find_header_containing(interior);
    if (header == nullptr) return false;
    if (out_base) *out_base = (void*)header;        // the mmap base
    if (out_size) *out_size = header->mapping_size;  // whole page-rounded region
    return true;
  }
}  // namespace alaska
