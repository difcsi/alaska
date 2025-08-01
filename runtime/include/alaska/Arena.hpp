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
#include <alaska/alaska.hpp>
#include <alaska/Logger.hpp>

namespace alaska {

  // An arena is a memory buffer that can only be allocated to, and all the memory is released when
  // the arena is destructed. You can also optionally clear it without destructing it, but this will
  // not free the memory. This is useful for when you want to reuse the arena for a different
  // purpose without having to reallocate the memory.
  // It is constructed with a "size" and a boolean on if it can grow or not. If an arena grows, we
  // simply chain the blocks together.
  class Arena {
    struct Block {
      Block* next;
      size_t used;
      size_t size;
      char data[0];

      void* allocate(size_t size) {
        if (used + size > this->size) {
          return nullptr;
        }
        void* ptr = &data[used];
        used += size;
        return ptr;
      }
      void clear() { used = 0; }
    };

   public:
    static const size_t DEFAULT_ARENA_SIZE = 4096 * 512;
    Arena(size_t arena_size = DEFAULT_ARENA_SIZE, bool can_grow = true)
        : m_arena_size(arena_size)
        , m_can_grow(can_grow) {
      // new_block();  // allocate the first block.
    }

    ~Arena() {
      Block* block = m_current_block;
      while (block) {
        Block* next = block->next;
        alaska_internal_free(block);
        block = next;
      }
    }


    void* push(size_t size, bool zero = false) {
      if (unlikely(m_current_block == nullptr)) {
        this->new_block();
      }

      // TODO: edge case!
      // if (size > (m_arena_size - sizeof(Block))) {
      //   return nullptr;
      // }
      void* ptr = m_current_block->allocate(size);
      if (ptr == nullptr) {
        if (m_can_grow) {
          new_block();
          ptr = m_current_block->allocate(size);
        } else {
          return nullptr;
        }
      }
      // unlikely
      if (zero) memset(ptr, 0, size);
      return ptr;
    }

    template <typename T>
    inline T* push() {
      auto p = (T*)push(sizeof(T));
      ::new (p) T();
      return p;
    }

    template <typename T>
    inline T* pushArray(size_t count) {
      auto p = (T*)push(sizeof(T) * count);
      for (size_t i = 0; i < count; i++) {
        ::new (&p[i]) T();
      }
      return p;
    }


    inline void disable_growth(void) { m_can_grow = false; }
    inline size_t remaining(void) const { return m_current_block->size - m_current_block->used; }

    void clear(void) {
      Block* block = m_current_block;
      while (block) {
        block->clear();
        block = block->next;
      }
    }


   private:
    Block* new_block(void) {
      alaska::printf("Creating new arena block of size %zu\n", m_arena_size);
      Block* new_block = (Block*)alaska_internal_malloc(sizeof(Block) + m_arena_size);
      new_block->next = nullptr;
      new_block->used = 0;
      new_block->size = m_arena_size;
      if (m_current_block) {
        m_current_block->next = new_block;
      }
      m_current_block = new_block;
      return new_block;
    }

    Block* m_current_block = nullptr;
    size_t m_arena_size = 0;
    bool m_can_grow = 0;
  };
}  // namespace alaska
