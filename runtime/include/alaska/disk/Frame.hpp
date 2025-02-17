/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */

#pragma once


#include <stdint.h>
#include <stdlib.h>
#include <alaska/utils.h>

#include <alaska/disk/Disk.hpp>

namespace alaska::disk {


  /*
   * A frame is an in-memory representation of a page from the disk.
   * The BufferPool managed an internal array of these frames and does
   * some fancy LRU replacement to keep most recently used paged in memory.
   * They must be used through a "FrameGuard"
   */
  class Frame final {
   public:
    // If the frame is referenced since the last time the clock looked.
    bool ref = false;
    bool dirty = false;
    bool valid = false;

    void *memory;  // Backing data
    uint64_t page_id = 0;
    int refcount = 0;


    inline void clear(void) {
      ref = false;
      dirty = false;
      valid = false;
      page_id = 0;
    }

    inline void reset(uint64_t page_id) {
      this->ref = false;
      this->valid = true;
      this->dirty = false;
      this->page_id = page_id;
      this->refcount = 0;
    }

   protected:
    friend class FrameGuard;
    friend class BufferPool;
    int get_refcount(void) { return atomic_get(this->refcount); }
    void inc_refcount(void) { atomic_inc(this->refcount, 1); }
    void dec_refcount(void) { atomic_dec(this->refcount, 1); }
  };



  // A frame guard is a RAII wrapper around a frame, tracking
  // that the frame is currently being used by some C++ code.
  // It also provides a convenient way to access the memory
  // in a constant or mutable way.
  class FrameGuard final {
   protected:
    Frame *frame = NULL;

    void set(Frame *new_frame) {
      auto old_frame = this->frame;
      if (new_frame) {
        new_frame->inc_refcount();
      }
      this->frame = new_frame;
      if (old_frame) {
        old_frame->dec_refcount();
      }
    }

   public:
    inline FrameGuard(Frame *frame) { set(frame); }
    inline FrameGuard(FrameGuard &other) { set(other.frame); }
    inline FrameGuard(FrameGuard &&other) {
      set(other.frame);
      other.set(nullptr);
    }
    inline FrameGuard &operator=(const FrameGuard &other) {
      set(other.frame);
      return *this;
    }

    inline ~FrameGuard(void) { set(nullptr); }


    inline uint64_t page_id(void) { return frame->page_id; }

    template <typename T>
    const T *get(off_t byte_offset = 0) {
      frame->ref = true;
      return (const T *)((uint8_t *)frame->memory + byte_offset);
    }
    template <typename T>
    T *getMut(off_t byte_offset = 0) {
      frame->ref = true;
      frame->dirty = true;
      return (T *)((uint8_t *)frame->memory + byte_offset);
    }

    void wipePage(void) { memset(getMut<void>(), 0, page_size); }
  };

}  // namespace alaska::disk