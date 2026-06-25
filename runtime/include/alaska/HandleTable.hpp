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
                            public alaska::InternalHeapAllocated {
    slabidx_t idx;                           // Which slab is this?
    HandleSlabState state = SlabStateEmpty;  // What is the state of this slab?
    HandleTable &table;                      // Which table does this belong to?

    HandleSlab *next = nullptr;                // The next slab in the queue
    HandleSlab *prev = nullptr;                // The previous slab in the queue
    HandleSlabQueue *current_queue = nullptr;  // What queue is this slab in?


    // -- Methods --
    HandleSlab(HandleTable &table, slabidx_t idx);
    void update_state(void);  // Update the state of this slab
    void dump(FILE *stream);  // Dump this slab's debug info to a file

    alaska::Mapping *alloc(void);             // Allocate a mapping from this slab
    void release_remote(alaska::Mapping *m);  // Return a mapping back to this slab (remote)
    void release_local(alaska::Mapping *m);   // Return a mapping back to this slab (local)
    void mlock(void);                         // `mlock` the memory behind this slab

    SizedAllocator allocator;
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
    static constexpr size_t slab_size = alaska::page_size;
    static constexpr size_t slab_capacity = slab_size / sizeof(alaska::Mapping);
    static constexpr size_t initial_capacity = 16;


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

    // True only for a slot that currently holds a LIVE handle: in-range, not freed, and
    // pointing at a real backing object. This is STRONGER than valid_handle() + !is_free(),
    // and the distinction is load-bearing for the conservative heap scanners.
    //
    // A Mapping slot sitting on the handle allocator's free list (ShardedFreeList) -- or one
    // still in the bump region, never handed out -- has its 8-byte word overwritten with a
    // raw `next` link (another slot's address, or NULL). That link leaves the invl/free bit
    // (bit 62) CLEAR, so is_free() reports such a slot as LIVE. A conservative scan that
    // trusts is_free() will then hand the slot to inc/dec_refcount, whose whole-word CAS
    // rewrites the refcount field (bits 47-60) of what is actually a free-list link --
    // corrupting the allocator's free list and crashing the next allocation in
    // ShardedFreeList::pop.
    //
    // We catch it cheaply: a free-list link decodes (low 47 bits) to an address INSIDE this
    // table (or NULL), whereas a genuine backing pointer is an object-heap / huge allocation
    // that never points into the handle table. So reject any mapping whose backing falls in
    // [m_table, m_table + slab_count*slab_size). Every conservative scanner -- dec-on-free,
    // copy-inc, and the cycle collector's child walk -- must gate on this, not is_free().
    inline bool is_live_handle(alaska::Mapping *m) const {
      if (m == nullptr) return false;
      if (!valid_handle(m)) return false;
      if (m->is_free()) return false;
      uintptr_t backing = (uintptr_t)m->get_pointer();
      if (backing == 0) return false;
      uintptr_t base = (uintptr_t)m_table;
      uintptr_t end = base + (uintptr_t)m_slabs.size() * slab_size;
      if (backing >= base && backing < end) return false;  // a free-list link, not a backing
      return true;
    }

    // A teardown snapshot of how many handles the table is holding: `total` live
    // (allocated, not free) handles, and how many of those have a nonzero
    // reference count. `nonzero_refcount` is always 0 when refcounting is
    // compiled out (no handle carries a count then).
    struct HandleCensus {
      size_t total = 0;
      size_t nonzero_refcount = 0;
    };

    // Walk every mapping slot in every slab under the table lock and tally the
    // census above. Used by the measurement build's exit-time dump to report how
    // many handles still existed -- and how many still had references -- at
    // teardown (a refcount-leak / never-freed-handle indicator).
    HandleCensus census_handles();


    // Free/release *some* mapping
    void put(alaska::Mapping *m, alaska::ThreadCache *owner = (alaska::ThreadCache *)0x1000UL);

    void *get_base(void) const { return (void *)m_table; }


    void enable_mlock() { do_mlock = true; }

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
}  // namespace alaska
