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

#include <alaska/utils.h>
#include <stdio.h>

namespace alaska {

  // This structure abstracts the concept of a block in the free list.
  // This is needed so
  struct DefaultFreeListBlock {
    DefaultFreeListBlock *next;

    inline void setNext(DefaultFreeListBlock *n) { next = n; }
    inline DefaultFreeListBlock *getNext(void) const { return next; }

    inline void markFreed(void) {}
    inline void markAllocated(void) {}
  };

  // This class provides a singular abstraction for managing local/remote free lists of simple
  // block-like objects in memory. It works entirely using `void*` pointers to blocks of memory,
  // and doesn't really care what the memory is, so long as it is at least 8 bytes big.
  template <typename Block = DefaultFreeListBlock>
  class ShardedFreeList final {
   public:
    // Pop from the local free list. Return null if the local free list is empty
    inline Block *pop(void) {
      auto *b = local_free;
      if (unlikely(b != nullptr)) {
        num_local_free--;
        local_free = local_free->next;
      }
      b->markAllocated();
      return b;
    }


    inline bool has_local_free(void) const { return local_free != nullptr; }
    // WEAK ORDERING!
    inline bool has_remote_free(void) const { return remote_free != nullptr; }


    inline long num_free(void) const {
      // atomics?
      return num_local_free + num_remote_free;
    }

    // Ask the free list to swap remote_free into the local_free list atomically.
    inline void swap(void) {
      if (local_free != nullptr) return;  // Sanity!
      do {
        this->local_free = this->remote_free;
      } while (!__atomic_compare_exchange_n(
          &this->remote_free, &this->local_free, nullptr, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

      // ??? ATOMICS ???
      num_local_free += num_remote_free;
      atomic_set(num_remote_free, 0);
    }


    inline void free_local(void *p) {
      auto *b = (Block *)p;
      b->next = local_free;
      b->markFreed();
      local_free = b;
      num_local_free++;
    }

    __attribute__((noinline))
    inline void free_remote(void *p) {
      auto *block = (Block *)p;
      Block **list = &remote_free;
      // TODO: NOT SURE ABOUT THE CONSISTENCY OPTIONS HERE
      do {
        block->next = *list;
      } while (!__atomic_compare_exchange_n(
          list, &block->next, block, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

      block->markFreed();
      // I don't like this atomic.
      atomic_inc(num_remote_free, 1);
    }

    // Wipe out the free lists.
    void reset() {
      local_free = remote_free = NULL;
      num_local_free = num_remote_free = 0;
    }

   private:
    Block *local_free = nullptr;
    Block *remote_free = nullptr;


    long num_local_free = 0;
    long num_remote_free = 0;
  };




}  // namespace alaska
