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
#include <alaska/alaska.hpp>

namespace alaska::disk {


  /*
   * A frame is an in-memory representation of a page from the disk.
   * The BufferPool managed an internal array of these frames and does
   * some fancy LRU replacement to keep most recently used paged in memory.
   * They must be used through a "FrameGuard"
   */
  class Frame final : public alaska::InternalHeapAllocated {
   public:
    // If the frame is referenced since the last time the clock looked.
    bool ref = false;
    bool dirty = false;
    bool valid = false;

    void *memory;  // Backing data
    uint64_t page_id = 0;
    int refcount = 0;
    uint32_t latch = 0;


    // Simple LRU chain list. if lru_prev == null, this is the head of the list.
    // if lru_next == null, this is the tail of the list (newest)
    Frame *lru_next, *lru_prev;


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
      this->latch = 0;  // ASSERT?
    }

    void take_latch(void) {
      while (__atomic_test_and_set(&latch, __ATOMIC_ACQUIRE)) {
      }
    }

    void release_latch(void) { __atomic_clear(&latch, __ATOMIC_RELEASE); }

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
      if (old_frame) old_frame->dec_refcount();
      this->frame = new_frame;
    }

    friend class Latch;

   public:
    inline FrameGuard(void) { set(nullptr); }
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


    inline uint64_t page_id(void) {
      if (!frame) abort();
      return frame->page_id;
    }


    inline void drop(void) { set(nullptr); }

    template <typename T>
    const T *get(off_t byte_offset = 0) {
      if (!frame) abort();
      frame->ref = true;
      return (const T *)((uint8_t *)frame->memory + byte_offset);
    }
    template <typename T>
    T *getMut(off_t byte_offset = 0) {
      if (!frame) abort();
      frame->ref = true;
      frame->dirty = true;
      return (T *)((uint8_t *)frame->memory + byte_offset);
    }

    void wipePage(void) {
      if (!frame) abort();
      memset(getMut<void>(), 0, page_size);
    }

    operator bool(void) { return frame != nullptr; }
  };


  class Latch {
   protected:
    FrameGuard guard;

   public:
    Latch() {
      // Do nothing.
    }

    Latch(FrameGuard &guard)
        : guard(guard) {
      if (guard) guard.frame->take_latch();
    }

    Latch(Latch &&other)
        : guard(ck::move(other.guard)) {}


    Latch &operator=(Latch &&other) {
      guard = ck::move(other.guard);
      return *this;
    }

    ~Latch() {
      if (guard) guard.frame->release_latch();
    }
  };


  template <typename T>
  class GuardedMut {
    FrameGuard frame;
    T *data;

   public:
    GuardedMut(FrameGuard frame, off_t byte_offset = 0)
        : frame(frame) {
      this->data = frame.getMut<T>(byte_offset);
    }
    T &operator*() { return *data; }
    T *operator->() { return data; }

    FrameGuard &guard(void) { return frame; }
  };


  template <typename T>
  class Guarded {
    FrameGuard frame;
    off_t byte_offset;
    const T *data;


   public:
    Guarded(FrameGuard frame, off_t byte_offset = 0)
        : frame(frame)
        , byte_offset(byte_offset) {
      this->data = frame.get<T>(byte_offset);
    }

    const T &operator*() { return *data; }
    const T *operator->() { return data; }


    // Get a mutable view of the same data.
    // Note that you can't go the other way (mut -> immut)
    GuardedMut<T> mut(void) { return GuardedMut<T>(frame, byte_offset); }


    FrameGuard &guard(void) { return frame; }
  };

  template <typename Fn>
  void withLatch(FrameGuard &frame, Fn &&fn) {
    Latch latch(frame);
    fn();
  }


}  // namespace alaska::disk