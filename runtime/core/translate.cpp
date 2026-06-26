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
#include <execinfo.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <sys/mman.h>
#include <unistd.h>

#include <alaska.h>
#include <alaska/alaska.hpp>
#include <alaska/config.h>
#include <alaska/utils.h>
#include <alaska/Logger.hpp>
#include "alaska/Runtime.hpp"
#include <dlfcn.h>

/**
 * Note: This file is inlined by the compiler to make locks faster.
 * Do not declare any global variables here, as they may get overwritten
 * or duplciated needlessly. (Which can lead to linker errors)
 */



extern "C" {
extern int __LLVM_StackMaps __attribute__((weak));
}

#define APPLY_OFFSET(mapped, bits) \
  (void *)((uint64_t)mapped + ((uint64_t)bits & ((1LU << ALASKA_SIZE_BITS) - 1)))

extern "C" void *alaska_translate_uncond(void *ptr) {
  int64_t bits = (int64_t)ptr;

  auto m = alaska::Mapping::from_handle(ptr);
  // Pull the address from the mapping
  void *mapped = m->get_pointer_fast();
  ptr = APPLY_OFFSET(mapped, bits);
  return ptr;
}

// Pin/unpin a handle so the relocating GC will not move its backing object.
// `set_pinned` is consulted by the barrier (see rt/barrier.cpp). Both are
// no-ops on non-handles (from_handle_safe returns null), so callers may pass an
// arbitrary pointer. Exposed as C symbols so header-only consumers can bind
// them weakly (see stackscan handle_query.h: ss_pin/ss_unpin).
extern "C" void alaska_pin(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return;
  m->set_pinned(true);
}

extern "C" void alaska_unpin(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return;
  m->set_pinned(false);
}

// Query whether a handle is currently pinned. Returns 0 on non-handles.
extern "C" int alaska_is_pinned(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return 0;
  return m->is_pinned() ? 1 : 0;
}

// TODO: we don't use this anymore. Do we need it?
void *alaska_translate_escape(void *ptr) {
  if (ptr == (void *)-1UL) {
    return ptr;
  }
  return alaska_translate(ptr);
}



#ifdef ALASKA_HTLB_SIM
extern void alaska_htlb_sim_track(uintptr_t handle);
#endif



extern "C" void do_handle_fault(void) { return; }


// This function is a marker, and just gets removed
// after the compiler does some magic to merge them.
__attribute__((noinline)) extern "C" void alaska_do_handle_fault_check(
    void *ptr, void *handle, void *retry);

void *alaska_translate(void *ptr) {
#ifdef ALASKA_HTLB_SIM
  alaska_htlb_sim_track((uintptr_t)ptr);
#endif

  int64_t bits = (int64_t)ptr;
  int64_t mapped_bits;
  if (unlikely(bits >= 0 || bits == -1)) {
    return ptr;
  }


  // Grab the mapping from the runtime
  auto m = alaska::Mapping::from_handle(ptr);


retry_translation:

  // Grab the pointer
  void *mapped = m->get_pointer_fast();

  // alaska_do_handle_fault_check(mapped, ptr, &&retry_translation);
  // mapped_bits = (int64_t)mapped;
  // if (unlikely(mapped_bits < 0)) {
  //   // asm volatile(
  //   //     "mov %0, %%rax\n"     // Move the label address into the RAX register
  //   //     "mov %%rax, 0(%0)\n"  // Move the label address into the RAX register
  //   //     :
  //   //     : "r"(&&retry_translation), "r"(mapped)  // Input operand
  //   //     : "%rax"                                 // Clobbers RAX
  //   // );
  //   alaska::do_handle_fault(bits);
  //   goto retry_translation;
  // }

  // load from the address for some reason
  uint8_t v;
  v = *(volatile uint8_t *)mapped;

  // Apply the offset from the pointer
  void *result = APPLY_OFFSET(mapped, bits);
  return result;
}

void alaska_release(void *ptr) {
  // This function is just a marker that `ptr` is now dead (no longer used)
  // and should not have any real meaning in the runtime
}

extern "C" uint64_t alaska_barrier_poll();
extern "C" void alaska_safepoint(void) { alaska_barrier_poll(); }

// TODO:
extern "C" void *__alaska_leak(void *ptr) { return alaska_translate(ptr); }


#if ALASKA_ENABLE_REFCOUNT
// The reference-count INCREMENT barrier, which the compiler inserts on every heap
// pointer store (see compiler/passes/RefcountInc.cpp). It lives here -- in the
// translate bitcode that alaska-transform llvm-links --internalize into each
// module -- rather than in refcount.cpp (which is only ever in libalaska.so) so
// that each call site resolves to a LOCAL direct call with the whole-word CAS
// (Mapping::inc_refcount, now inline in alaska.hpp) inlined, instead of a PLT call
// into the shared library on every store. Only the rare bookkeeping case -- a
// handle reaching refcount 1 while it is still on the zero-refcount nullcount list
// -- is handed to an out-of-line slow path (it needs the lock-protected map, so it
// is not worth inlining and only fires after a prior dec-to-zero).
extern "C" void alaska_inc_refcount_nullcount(void *ptr);

// Deferred reference counting (Levanoni-Petrank). DEFERRAL IS A COMPILE-TIME CHOICE
// (ALASKA_ENABLE_DEFER_RC, set by the *-defer build preset): when on, the increment is appended
// to a per-thread log (alaska_defer_inc) and applied in a FIFO batch at the barrier
// (ThreadCache::drain_inc) instead of doing the scattered handle-table CAS here. alaska_defer_inc
// lives in libalaska -- this file is internalized into every module, so it must only REFERENCE
// it. When off, the barrier is byte-identical to the original eager path: no buffer, no branch.
#if ALASKA_ENABLE_DEFER_RC
extern "C" void alaska_defer_inc(alaska::Mapping *m);
#endif

extern "C" void alaska_inc_refcount(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);  // null / non-handle -> nullptr
  if (m == nullptr) return;
#if ALASKA_ENABLE_DEFER_RC
  alaska_defer_inc(m);
  return;
#elif ALASKA_ENABLE_CYCLE_COLLECTION
  bool resurrected;
  m->inc_refcount_gc(&resurrected);
  if (unlikely(resurrected)) alaska_inc_refcount_nullcount(ptr);
#else
  m->inc_refcount();
#endif
}

// The reference-count DECREMENT barrier (compiler-inserted on pointer overwrites:
// the old value at the store target is decremented before the store). Same inlining
// rationale as the increment above -- a local direct call with the CAS inlined
// rather than a PLT call into libalaska on every overwrite. The overwhelmingly
// common case in store-heavy code is decrementing the freshly-zeroed (null) slot
// before a first store, which returns inline here without touching the library.
// Routing a real decrement to the GC (nullcount list / cycle-candidate set) only
// happens in GC builds and is handed to an out-of-line slow path.
#if ALASKA_ENABLE_CYCLE_COLLECTION
extern "C" void alaska_dec_refcount_slow(void *ptr, int new_count);
#endif

extern "C" void alaska_dec_refcount(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);  // null / non-handle -> nullptr
  if (m == nullptr) return;
  auto new_count = m->dec_refcount();
#if ALASKA_ENABLE_CYCLE_COLLECTION
  alaska_dec_refcount_slow(ptr, (int)new_count);
#endif
}
#endif
