/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2023, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2023, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <alaska/utils.h>
#include <alaska/list_head.h>
#include <alaska/liballoc.h>
#include <alaska/config.h>
#include <alaska/Logger.hpp>
#include <alaska/EventCounters.hpp>

#include <ck/utility.h>



#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void show_string(const char *msg) { write(1, msg, strlen(msg)); }


#define HANDLE_ADDRSPACE __attribute__((address_space(1)))

// Fwd decl stuff
namespace alaska {
  class Mapping;
}

extern "C" {
// src/translate.cpp
void *alaska_encode(alaska::Mapping *m, off_t offset);
void *alaska_translate_escape(void *ptr);
void *alaska_translate(void *ptr);
void alaska_release(void *ptr);
void *alaska_ensure_present(alaska::Mapping *m);
}

namespace alaska {

  extern long translation_hits;
  extern long translation_misses;

  using handle_id_t = uint64_t;


  class Mapping {
   private:
    // Represent the fact that a handle is just a pointer w/ "important bit patterns"
    // as a simple union.
    //
    // Layout: the backing pointer lives in the *low* 47 bits so that it never
    // overlaps the pinned/invl/swap flags in the top three bits. (A userspace
    // pointer only ever uses the low 47 bits -- the lower half of a 48-bit
    // address space -- so 47 bits round-trips it exactly.) Keeping the pointer
    // and the flags disjoint is essential: an earlier layout packed a 48-bit
    // `value` into the high bits, so storing a heap pointer above 2^45 silently
    // flipped the invl/pinned bits, corrupting is_free()/is_pinned()/get_pointer().
    // The reference count occupies the bits between the pointer and the flags.
    union {
      struct {
        uint64_t value : 47;     // bits  0-46: the base pointer (offset added later)
        uint64_t refcount : 14;  // bits 47-60: reference count
        uint64_t pinned : 1;     // bit  61: this handle is pinned currently
        uint64_t invl : 1;       // bit  62: this handle is not mapped (ptr is a free list)
        uint64_t swap : 1;       // bit  63: this handle is swapped
      } rc __attribute__((packed));
      struct {
        uint64_t misc : 61;   // Some kind of extra info (usually just a pointer)
        uint64_t pinned : 1;  // If this handle is pinned currently.
        uint64_t invl : 1;    // This handle is not mapped. ptr is a free list
        uint64_t swap : 1;    // This handle is swapped
      } alt __attribute__((packed));
    };
  public:

   ALASKA_INLINE void *get_pointer(void) const {
      // The pointer occupies the low bits verbatim; the flags/refcount above it
      // are not part of the address.
      return (void *)(uint64_t)this->rc.value;
    }

    ALASKA_INLINE void *get_pointer_fast(void) const {
      return (void *)(uint64_t)this->rc.value;
    }

    inline void invalidate(void) {
#ifdef __riscv
      // Fence *before* the handle invalidation.
      __asm__ volatile("fence" ::: "memory");
      __asm__ volatile("csrw 0xc4, %0" ::"rK"((uint64_t)handle_id()) : "memory");
#endif
    }

    // Bit layout of the packed 8-byte word (see the union above):
    //   value  : bits  0-46   pointer (47 bits)
    //   misc   : bits  0-60   free-list link (alt view, 61 bits)
    //   refcnt : bits 47-60   reference count
    //   pinned : bit  61
    //   invl   : bit  62
    //   swap   : bit  63
    // Every writer below mutates this word with an atomic RMW/store so it never
    // tears against the concurrent refcount CAS (Runtime.cpp) or a barrier
    // handler flipping `pinned` from another thread. Readers on the hot path
    // (get_pointer/is_free) stay plain loads -- a naturally-aligned 8-byte read
    // is atomic on the supported targets, as the original code already relied on.
    static constexpr uint64_t kValueMask = (1ULL << 47) - 1;   // bits 0-46
    static constexpr uint64_t kMiscMask = (1ULL << 61) - 1;    // bits 0-60
    static constexpr uint64_t kPinnedBit = 1ULL << 61;
    static constexpr uint64_t kInvlBit = 1ULL << 62;
    // bit 63 (the otherwise-unused `swap` flag): a HINT, maintained by the refcount
    // runtime, that this handle is currently on the zero-refcount nullcount list.
    // It lets alaska_inc_refcount skip the (syscall-bracketed) nullcount-map removal
    // when the handle was never added -- the dominant case for allocate-and-store
    // workloads. The nullcount_map remains the source of truth; the reclaim path
    // re-validates every entry, so a drifted hint can never cause a wrong free.
    static constexpr uint64_t kOnNullcountBit = 1ULL << 63;

    void set_pointer(void *ptr) {
      // Write the pointer (low 47 bits) and clear invl, preserving refcount and
      // the pinned/swap flags via a CAS over the whole word.
      auto *w = reinterpret_cast<uint64_t *>(this);
      uint64_t old = __atomic_load_n(w, __ATOMIC_RELAXED);
      uint64_t neu;
      do {
        neu = old;
        neu = (neu & ~kValueMask) | ((uint64_t)ptr & kValueMask);
        neu &= ~kInvlBit;
      } while (!__atomic_compare_exchange_n(
          w, &old, neu, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
      invalidate();
    }


    // Get the next mapping in the free list. Returns NULL
    // if this isn't a free handle
    alaska::Mapping *get_next(void) {
      if (is_free()) return NULL;
      return (alaska::Mapping *)(uint64_t)alt.misc;
    }

    void set_next(alaska::Mapping *next) {
      // Free-list link: misc(bits 0-60)=next, invl set, everything else cleared.
      // Equivalent to the old reset()+misc/invl writes but as one atomic store.
      uint64_t neu = ((uint64_t)next & kMiscMask) | kInvlBit;
      __atomic_store_n(reinterpret_cast<uint64_t *>(this), neu, __ATOMIC_RELEASE);
      invalidate();
    }


    bool is_free(void) const { return alt.invl; }


    bool is_pinned(void) const {
      uint64_t w = __atomic_load_n(
          reinterpret_cast<uint64_t *>(const_cast<Mapping *>(this)), __ATOMIC_ACQUIRE);
      return (w & kPinnedBit) != 0;
    }
    void set_pinned(bool to) {
      auto *w = reinterpret_cast<uint64_t *>(this);
      if (to) {
        __atomic_fetch_or(w, kPinnedBit, __ATOMIC_ACQ_REL);
      } else {
        __atomic_fetch_and(w, ~kPinnedBit, __ATOMIC_ACQ_REL);
      }
    }

    // Nullcount-list membership hint (see kOnNullcountBit). Relaxed: it is only a
    // hint read by the refcount fast path; the lock-protected nullcount_map plus
    // the reclaim re-validation provide the actual correctness. Set/cleared by the
    // refcount runtime in lockstep with nullcount_map add/remove; cleared for free
    // by reset()/set_next() (a fresh/recycled slot is never on the list).
    bool is_on_nullcount(void) const {
      uint64_t w = __atomic_load_n(
          reinterpret_cast<uint64_t *>(const_cast<Mapping *>(this)), __ATOMIC_RELAXED);
      return (w & kOnNullcountBit) != 0;
    }
    void set_on_nullcount(bool to) {
      auto *w = reinterpret_cast<uint64_t *>(this);
      if (to) {
        __atomic_fetch_or(w, kOnNullcountBit, __ATOMIC_RELAXED);
      } else {
        __atomic_fetch_and(w, ~kOnNullcountBit, __ATOMIC_RELAXED);
      }
    }


    void reset(void) {
      // Clear everything: pointer, refcount, and flags. One atomic store so a
      // freed/recycled mapping starts clean without tearing a concurrent reader.
      __atomic_store_n(reinterpret_cast<uint64_t *>(this), 0ULL, __ATOMIC_RELEASE);
      invalidate();
    }

#if ALASKA_ENABLE_REFCOUNT
    // The reference count occupies bits 47-60 (14 bits) of the packed 8-byte word.
    // Mutation must be a CAS over the *whole* word: the collector reads refcounts
    // and the barrier handler flips the pinned bit (61) while mutators run, so a
    // plain bitfield ++/-- (a non-atomic RMW of the whole word) would tear against
    // a concurrent set_pinned()/set_pointer()/reset(). These are defined inline in
    // the header (rather than out-of-line in Runtime.cpp) so the compiler-inserted
    // increment barrier can inline the CAS instead of emitting a cross-library call
    // -- the increment runs on every heap pointer store (see RefcountInc).
    static constexpr unsigned kRefcountShift = 47;
    static constexpr uint64_t kRefcountMaxField = (1ULL << 14) - 1;            // 0x3FFF
    static constexpr uint64_t kRefcountFieldMask = kRefcountMaxField << kRefcountShift;

    // Atomically increment the reference count and return the new value.
    //
    // ORDERING: relaxed. A strong-reference increment needs only atomicity, not
    // synchronization -- the caller already holds a live reference to this handle
    // (it is storing that very pointer), so a happens-before edge keeping the object
    // alive already exists and the increment publishes nothing other code reads
    // (this is exactly why std::shared_ptr increments use memory_order_relaxed). The
    // whole-word CAS still prevents tearing against a concurrent set_pinned/
    // set_pointer regardless of ordering, and a stale relaxed load just loses the CAS
    // and retries. Decrement keeps ACQ_REL: the dec-to-zero must synchronize-with the
    // reclaimer. On x86 this is a no-op (lock cmpxchg is a full barrier); on the
    // weak-memory targets (ARM/RISC-V) it drops real acquire/release fences.
    ALASKA_INLINE int inc_refcount(void) {
      auto *w = reinterpret_cast<uint64_t *>(this);
      uint64_t old = __atomic_load_n(w, __ATOMIC_RELAXED);
      uint64_t neu;
      uint64_t nc;
      do {
        uint64_t rc = (old >> kRefcountShift) & kRefcountMaxField;
        nc = (rc + 1) & kRefcountMaxField;  // wrap like the old 14-bit bitfield did
        neu = (old & ~kRefcountFieldMask) | (nc << kRefcountShift);
      } while (!__atomic_compare_exchange_n(
          w, &old, neu, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
#if ALASKA_ENABLE_EVENT_COUNTERS
      // Counted at the single refcount-mutation point so the tally captures every
      // increment: compiler-inserted handle writes AND the allocator's birth/death
      // bumps (HandleSlab::alloc/release_*).
      alaska::events::inc_refcount_event();
#endif
#if ALASKA_ENABLE_CACHE_PROBE
      alaska::events::probe_mapping_line(reinterpret_cast<uintptr_t>(this));
#endif
      return (int)nc;
    }

    // Atomically decrement the reference count and return the new value.
    ALASKA_INLINE int dec_refcount(void) {
      auto *w = reinterpret_cast<uint64_t *>(this);
      uint64_t old = __atomic_load_n(w, __ATOMIC_ACQUIRE);
      uint64_t neu;
      uint64_t nc;
      do {
        uint64_t rc = (old >> kRefcountShift) & kRefcountMaxField;
        nc = (rc - 1) & kRefcountMaxField;  // wrap like the old 14-bit bitfield did
        neu = (old & ~kRefcountFieldMask) | (nc << kRefcountShift);
      } while (!__atomic_compare_exchange_n(
          w, &old, neu, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
#if ALASKA_ENABLE_EVENT_COUNTERS
      alaska::events::dec_refcount_event();
#endif
#if ALASKA_ENABLE_CACHE_PROBE
      alaska::events::probe_mapping_line(reinterpret_cast<uintptr_t>(this));
#endif
      return (int)nc;
    }

    // Get the current reference count.
    ALASKA_INLINE uint64_t get_refcount(void) {
      uint64_t w = __atomic_load_n(reinterpret_cast<uint64_t *>(this), __ATOMIC_ACQUIRE);
      return (w >> kRefcountShift) & kRefcountMaxField;
    }
#endif


    // Encode a handle into the representation used in the
    // top-half of a handle encoding
    ALASKA_INLINE uint64_t encode(void) const {
      auto out = (uint64_t)((uint64_t)this >> ALASKA_SQUEEZE_BITS);
      return out;
    }

    ALASKA_INLINE handle_id_t handle_id(void) const {
      uint64_t out = ((uint64_t)encode() << ALASKA_SIZE_BITS);
      return (out & ~(1UL << 63)) >> ALASKA_SIZE_BITS;
    }

    // Encode a mapping into a handle that can be later translated by
    // compiler-inserted means.
    ALASKA_INLINE void *to_handle(uint32_t offset = 0) const {
      // The table ensures the m address has bit 32 set. This meaning
      // decoding just checks is a 'is the top bit set?'
      uint64_t out = ((uint64_t)encode() << ALASKA_SIZE_BITS) + offset;
      // printf("encode %p %zu -> %p\n", this, offset, out);
      return (void *)out;
    }

    static void *translate(void *handle) {
      auto h = alaska::Mapping::from_handle_safe(handle);
      if (h == nullptr) return handle;
      return h->get_pointer();
    }

    // Extract an encoded mapping out of the bits of a handle. WARNING: this function does not
    // perform any checking, and will blindly translate any pointer regardless of if it really
    // contains a handle internally.
    static ALASKA_INLINE alaska::Mapping *from_handle(void *handle) {
      return (alaska::Mapping *)((uint64_t)handle >> (ALASKA_SIZE_BITS - ALASKA_SQUEEZE_BITS));
    }

    // Extract an encoded mapping out of the bits of a handle. This variant of the function
    // will first check if the pointer provided is a handle. If it is not, this method will
    // return null.
    static ALASKA_INLINE alaska::Mapping *from_handle_safe(void *ptr) {
      if (alaska::Mapping::is_handle(ptr)) {
        return alaska::Mapping::from_handle(ptr);
      }
      // Return null if the pointer is not really a handle
      return nullptr;
    }

    // Check if a pointer is a handle or not (is the top bit is set?)
    static ALASKA_INLINE bool is_handle(void *ptr) {
      return (int64_t)ptr < 0;  // This is quicker than shifting and masking :)
    }
  };


  static_assert(sizeof(alaska::Mapping) == 8,
                "Mapping must be 8 bytes to fit in a handle. Please fix this.");

  // runtime.cpp
  extern void record_translation_info(bool hit);



  // Construct an array of length `length` with default constructors
  template <typename T>
  T *make_object_array(size_t length) {
    // Allocate raw memory for the object
    auto ptr = (T *)alaska_internal_calloc(length, sizeof(T));

    for (size_t i = 0; i < length; i++) {
      // Use placement new to construct the object in the allocated memory
      ::new (ptr + i) T();
    }
    return ptr;
  }
  // Construct an array of length `length` with default constructors
  template <typename T>
  void delete_object_array(T *array, size_t length) {
    for (size_t i = 0; i < length; i++) {
      // call dtor
      array[i].~T();
    }
    alaska_internal_free((void *)array);
  }



  class InternalHeapAllocated {
   public:
    void *operator new(size_t size) { return alaska_internal_malloc(size); }
    void operator delete(void *ptr) { alaska_internal_free(ptr); }
  };

}  // namespace alaska
