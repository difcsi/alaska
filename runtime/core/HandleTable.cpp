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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include "alaska/utils.h"
#endif


#include <alaska/HandleTable.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/Logger.hpp>
#include <alaska/HeapPage.hpp>
#include <ck/lock.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <stdlib.h>

#ifdef ALASKA_YUKON
#define write_csr(reg, val) \
  ({ asm volatile("csrw %0, %1" ::"i"(reg), "rK"((uint64_t)val) : "memory"); })

#define CSR_TRACE 0xc7
static inline void mark_alloc(uint64_t handle_id) {
  return;
  write_csr(CSR_TRACE, handle_id);
  write_csr(CSR_TRACE, 0);
}

static inline void mark_free(uint64_t handle_id) {
  return;
  write_csr(CSR_TRACE, handle_id | (1ULL << 63));
  write_csr(CSR_TRACE, 0);
}
#endif


#ifdef __riscv
static int dev_alaska_fd = -1;
#endif

namespace alaska {


  int HandleTable::get_ht_fd(void) {
#ifdef ALASKA_YUKON
    return dev_alaska_fd;
#endif
    return -1;
  }

  //////////////////////
  // Handle Table
  //////////////////////
  HandleTable::HandleTable(const alaska::Configuration &config) {
    // We allocate a handle table to a fixed location. If that allocation fails,
    // we know that another handle table has already been allocated. Since we
    // don't have exceptions in this runtime we will just abort.

    uintptr_t table_start = config.handle_table_location;
    m_capacity = HandleTable::initial_capacity;


#ifdef __riscv
    if (dev_alaska_fd == -1) dev_alaska_fd = open("/dev/alaska", O_RDWR);

    m_table = (Mapping *)mmap((void *)table_start, m_capacity * HandleTable::slab_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dev_alaska_fd, 0);
    printf("Yukon: allocated handle table to %p with the kernel module!\n", m_table);

#else
    // Attempt to allocate the initial memory for the table.
    m_table = (Mapping *)mmap((void *)table_start, m_capacity * HandleTable::slab_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif

    // Validate that the table was allocated
    ALASKA_ASSERT(
        m_table != MAP_FAILED, "failed to allocate handle table. Maybe one is already allocated?");


    log_debug("handle table successfully allocated to %p with initial capacity of %lu", m_table,
        m_capacity);
  }

  HandleTable::~HandleTable() {
    // Release the handle table back to the OS
    int r = munmap(m_table, m_capacity * HandleTable::slab_size);
    if (r < 0) {
      log_error("failed to release handle table memory to the OS");
    } else {
      log_debug("handle table memory released to the OS");
    }


    for (auto &slab : m_slabs) {
      log_trace("deleting slab %p (idx: %lu)", slab, slab->idx);
      delete (slab);
    }
  }


  void HandleTable::grow() {
    auto new_cap = m_capacity * HandleTable::growth_factor;
    // Scale the capacity of the handle table
    log_debug("Growing handle table. New capacity: %lu, old: %lu", new_cap, m_capacity);

    // Grow the mmap region
    m_table = (alaska::Mapping *)mremap(
        m_table, m_capacity * HandleTable::slab_size, new_cap * HandleTable::slab_size, 0, m_table);

    m_capacity = new_cap;

    if (m_table == MAP_FAILED) {
      perror("failed to grow handle table.\n");
    }
    // Validate that the table was reallocated
    ALASKA_ASSERT(m_table != MAP_FAILED, "failed to reallocate handle table during growth");
  }

  HandleSlab *HandleTable::fresh_slab(ThreadCache *new_owner) {
    ck::scoped_lock lk(this->lock);

    slabidx_t idx = m_slabs.size();
    log_trace("Allocating a new slab at idx %d", idx);

    if (idx >= this->capacity()) {
      log_debug("New slab requires more capacity in the table");
      grow();
      ALASKA_ASSERT(idx < this->capacity(), "failed to grow handle table");
    }

    // Allocate a new slab using the system allocator.
    // auto *sl = alaska::make_object<HandleSlab>(*this, idx);
    auto *sl = new HandleSlab(*this, idx);
    if (do_mlock) sl->mlock();
    sl->set_owner(new_owner);

    // Add the slab to the list of slabs and return it
    m_slabs.push(sl);

    // // Skip slab 0 for various reasons. We really need to figure out a better
    // // way to handle "invalid" handle IDs such as handle 0.
    // if (idx == 0) {
    //   printf("Skipping slab 0\n");
    //   return fresh_slab(new_owner);
    // }

    return sl;
  }


  HandleSlab *HandleTable::new_slab(ThreadCache *new_owner) {
    {
      ck::scoped_lock lk(this->lock);


      // printf("new slab %p\n", new_owner);
      // dump(stderr);

      // TODO: PERFORMANCE BAD HERE. POP FROM A LIST!
      for (auto *slab : m_slabs) {
        log_trace("Attempting to allocate from slab %p (idx %lu)", slab, slab->idx);
        // if (slab->idx == 0) continue;
        if ((slab->get_owner() == nullptr || slab->get_owner() == new_owner) &&
            slab->allocator.num_free() > 0) {
          slab->set_owner(new_owner);
          return slab;
        }
      }
    }

    return fresh_slab(new_owner);
  }



  void HandleTable::dump(FILE *stream) {
    // // Dump the handle table in a nice debug output
    fprintf(stream, "Handle Table: ");
    // log_info(" - Size: %zu bytes\n", m_capacity * HandleTable::slab_size);
    for (auto *slab : m_slabs) {
      bool has_owner = slab->get_owner() != nullptr;
      long avail = slab->allocator.num_free();


      float avail_frac = avail / (float)slab_capacity;
      float used_frac = 1.0 - avail_frac;

      if (has_owner) {
        fprintf(stream, "\033[48;2;0;0;%dm owned", (int)(255 * used_frac));
      } else {
        fprintf(stream, "\033[48;2;%d;0;0m unowned", (int)(255 * used_frac));
      }

      // printf("%7.2f ", 100.0 * used_frac);
      fprintf(stream, "%7lu (%5.1f%%)", avail, avail_frac * 100.0f);
      fprintf(stream, "\e[0m ");
      // slab->dump(stream);
    }
    fprintf(stream, "\n");
  }


  bool HandleTable::valid_handle(Mapping *m) const {
    auto idx = mapping_slab_idx(m);
    if (idx > (slabidx_t)m_slabs.size()) {
      return false;
    }

    auto *slab = m_slabs[idx];
    return slab->allocator.is_allocated(m);
  }


  //////////////////////
  // Handle Slab Queue
  //////////////////////
  void HandleSlabQueue::push(HandleSlab *slab) {
    slab->current_queue = this;

    // Initialize
    if (head == nullptr and tail == nullptr) {
      head = tail = slab;
    } else {
      tail->next = slab;
      slab->prev = tail;
      tail = slab;
    }
  }

  HandleSlab *HandleSlabQueue::pop(void) {
    if (head == nullptr) return nullptr;

    auto *slab = head;
    head = head->next;
    if (head != nullptr) {
      head->prev = nullptr;
    } else {
      tail = nullptr;
    }
    slab->prev = slab->next = nullptr;
    slab->current_queue = nullptr;
    return slab;
  }


  void HandleSlabQueue::remove(HandleSlab *slab) {
    if (slab->prev != nullptr) {
      slab->prev->next = slab->next;
    } else {
      head = slab->next;
    }

    if (slab->next != nullptr) {
      slab->next->prev = slab->prev;
    } else {
      tail = slab->prev;
    }

    slab->prev = slab->next = nullptr;

    slab->current_queue = nullptr;
  }




  //////////////////////
  // Handle Slab
  //////////////////////

  HandleSlab::HandleSlab(HandleTable &table, slabidx_t idx)
      : table(table)
      , idx(idx) {


    auto start = table.get_slab_start(idx);
    auto capacity = HandleTable::slab_capacity;

    allocator.configure(start, sizeof(alaska::Mapping), capacity);


    // If we are the first slab, we need to ask the allocator to
    // allocate one handle, so that handle id 0 is "leaked" and will
    // not be used by any application. This is to maintain the invariant
    // that handle id 0 is a "invalid" or "null" handle in the runtime.
    if (idx == 0) {
      auto *p = allocator.alloc();
      if (p != start) {
        fprintf(stderr,
            "Failed to allocate first handle in slab 0. Allocator returned %p, expected %p\n", p,
            start);
        abort();
      }
    }
  }



  Mapping *HandleSlab::alloc(void) {
    auto *m = (Mapping *)allocator.alloc();

    if (unlikely(m == nullptr)) return nullptr;
    return m;
  }


  void HandleSlab::mlock(void) {
    auto start = table.get_slab_start(idx);
    ::mlock((void *)start, sizeof(alaska::Mapping) * HandleTable::slab_capacity);
  }



  void HandleSlab::dump(FILE *stream) {
    log_info("Slab %4zu | ", idx);
    log_info("st %d | ", state);

    auto owner = this->get_owner();
    log_info("owner: %4d | ", owner ? owner->get_id() : -1);
    log_info("free %4zu | ", allocator.num_free());
    log_info("\n");
  }
}  // namespace alaska
