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
#include <alaska/alaska.hpp>
#include <alaska/OwnedBy.hpp>
#include <alaska/HeapPage.hpp>
#include <ck/vec.h>
#include <ck/lock.h>
#include <alaska/SizedAllocator.hpp>
#include <alaska/Configuration.hpp>

namespace alaska {

  using slabidx_t = size_t;

  class HandleTable;
  class HandleSlabQueue;
  class ThreadCache;


  enum HandleSlabState {
    SlabStateEmpty,
    SlabStatePartial,
    SlabStateFull,
  };


  // A handle slab is a slice of the handle table that is used to allocate
  // mappings. It is a fixed size, and no two threads will allocate from the
  // same slab at the same time.
  struct HandleSlab final : public alaska::OwnedBy<alaska::ThreadCache>,
                            public alaska::PersistentAllocation {
   private:
    alaska::Mapping *start;
    alaska::Mapping *end;
    alaska::Mapping *next_free;  // Bump allocator.

    alaska::ShardedFreeList<DefaultFreeListBlock> free_list;  // A free list for tracking releases

   public:
    slabidx_t idx;                           // Which slab is this?
    HandleSlabState state = SlabStateEmpty;  // What is the state of this slab?
    HandleTable &table;                      // Which table does this belong to?

    HandleSlab *next = nullptr;                // The next slab in the queue
    HandleSlab *prev = nullptr;                // The previous slab in the queue
    HandleSlabQueue *current_queue = nullptr;  // What queue is this slab in?


    // -- Methods --
    HandleSlab(HandleTable &table, slabidx_t idx);
    void dump(FILE *stream);  // Dump this slab's debug info to a file


    // Implemented at the bottom of this file...
    alaska::Mapping *alloc(void);             // Allocate a mapping from this slab
    void release_remote(alaska::Mapping *m);  // Return a mapping back to this slab (remote)
    void release_local(alaska::Mapping *m);   // Return a mapping back to this slab (local)
    void mlock(void);                         // `mlock` the memory behind this slab


    bool contains(alaska::Mapping *m) const {
      // Check if the mapping is within the bounds of this slab
      return (m >= start && m < end);
    }

    bool allocated(alaska::Mapping *m) const {
      void *ptr = m->get_pointer();
      if (contains((alaska::Mapping *)ptr)) {
        return false;
      }
      return true;
    }

    // Implmented in HandleTable.cpp, this function cannot be inlined and is meant
    // to be used when the local free list is empty.
    alaska::Mapping *alloc_slow(void);

    size_t num_free(void) const { return free_list.num_free() + (end - next_free); }
    size_t capacity(void) const { return end - start; }
  };




  // A handle slab list is a doubly linked list of handle slabs.
  // It is used to keep track of groupings of slabs.
  class HandleSlabQueue final {
   public:
    void push(HandleSlab *slab);
    HandleSlab *pop(void);
    inline bool empty(void) const { return head == nullptr; }

    // remove a slab from the queue without popping it
    void remove(HandleSlab *slab);

   private:
    HandleSlab *head = nullptr;
    HandleSlab *tail = nullptr;
  };

  // This is a class which manages the mapping from pages in the handle table to slabs. If a
  // handle table is already allocated, this class will panic when being constructed.
  // In the actual runtime implementation, there will be a global instance of this class.
  class HandleTable final {
   public:
    static constexpr size_t slab_size = 1 << 21;  // 2MB
    static constexpr size_t slab_capacity = slab_size / sizeof(alaska::Mapping);
    static constexpr size_t initial_capacity = 512;
    static constexpr size_t handle_count = (1UL << (63 - ALASKA_SIZE_BITS)) - 1;

    static constexpr size_t max_slab_count = handle_count / slab_capacity;

    HandleTable(const alaska::Configuration &config);
    ~HandleTable(void);

    // Allocate a fresh slab, resizing the table if necessary.
    alaska::HandleSlab *fresh_slab(ThreadCache *new_owner);
    // Get *some* unowned slab, the amount of free entries currently doesn't really matter.
    alaska::HandleSlab *new_slab(ThreadCache *new_owner);
    alaska::HandleSlab *get_slab(slabidx_t idx);
    // Given a mapping, return the index of the slab it belongs to.
    slabidx_t mapping_slab_idx(Mapping *m) const;

    auto slab_count() const { return m_slabs.size(); }
    auto capacity() const { return m_capacity; }

    void dump(FILE *stream);

    bool valid_handle(alaska::Mapping *m) const;


    inline alaska::HandleSlab *get_slab(alaska::Mapping *m) {
      alaska::HandleSlab *slab = *(alaska::HandleSlab **)((uintptr_t)m & ~(alaska::page_size - 1));
      return slab;
    }

    // Free/release *some* mapping
    inline void put(
        alaska::Mapping *m, alaska::ThreadCache *owner = (alaska::ThreadCache *)0x1000UL) {
      alaska::HandleSlab *slab = get_slab(m);
      if (slab->is_owned_by(owner)) {
        slab->release_local(m);
      } else {
        slab->release_remote(m);
      }
    }

    void *get_base(void) const { return (void *)m_table; }


    void enable_mlock() { do_mlock = true; }

    const ck::vec<alaska::HandleSlab *> &get_slabs(void) const { return m_slabs; }

    static int get_ht_fd(void);  // only valid on riscv under yukon

   protected:
    friend HandleSlab;

    inline alaska::Mapping *get_slab_start(slabidx_t idx) {
      return (alaska::Mapping *)((uintptr_t)m_table + idx * slab_size);
    }

    inline alaska::Mapping *get_slab_end(slabidx_t idx) {
      return (alaska::Mapping *)((uintptr_t)m_table + (idx + 1) * slab_size);
    }


   private:
    void grow();
    bool do_mlock = false;

    // A lock for the handle table
    ck::mutex lock;
    // How many slabs this table can hold (how big is the mmap region)
    uint64_t m_capacity;
    // The actual memory for the mmap region.
    alaska::Mapping *m_table;


    // How much the table increases it's capacity by each time it grows.
    static constexpr int growth_factor = 2;

    ck::vec<alaska::HandleSlab *> m_slabs;
  };



  inline HandleSlab *HandleTable::get_slab(slabidx_t idx) {
    // ck::scoped_lock lk(this->lock);

    log_trace("Getting slab %d", idx);
    if (idx >= (slabidx_t)m_slabs.size()) {
      log_trace("Invalid slab requeset!");
      return nullptr;
    }
    return m_slabs[idx];
  }

  inline slabidx_t HandleTable::mapping_slab_idx(Mapping *m) const {
    auto byte_distance = (uintptr_t)m - (uintptr_t)m_table;
    return byte_distance / HandleTable::slab_size;
  }




  inline alaska::Mapping *HandleSlab::alloc(void) {
    // 1. Attempt to allocate a mapping from the free list.
    auto *m = (alaska::Mapping *)free_list.pop();
    // 2. If that fails, drop out to the slow path
    if (unlikely(m == nullptr)) {
      m = alloc_slow();
    }
    return m;
  }

  inline void HandleSlab::release_remote(Mapping *m) { free_list.free_remote((void *)m); }
  inline void HandleSlab::release_local(Mapping *m) { free_list.free_local((void *)m); }
}  // namespace alaska
