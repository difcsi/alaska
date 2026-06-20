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

#include <ck/map.h>
#include <ck/lock.h>
#include <ck/vec.h>
#include <alaska/alaska.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/gc_bitmaps.hpp>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

namespace alaska::gc { extern unsigned long g_mark_count; }  // diagnostic

ck::HashTable<void*> nullcount_map;

// nullcount_map tracks handles whose reference count has reached zero. Mutators
// mutate/read it (inc/dec_refcount, size); the in-barrier reclaim iterates it.
// Every access is serialized by this lock (ck::HashTable rehashes on set/remove,
// invalidating a concurrent iterator).
//
// BARRIER SAFETY: a thread parked at Alaska's stop-the-world barrier while holding
// this lock would block every other thread from acquiring it, and any thread that
// blocked on it could never reach a safepoint to join -- a deadlock. Anchorage
// avoids this for its own locks by pre-acquiring them all before signalling, but
// that does not work here: a mutator about to call a nullcount op during a barrier
// would still block on a pre-held lock and never join. Instead, every
// MUTATOR-callable access blocks the barrier signal (SIGUSR2) across the (tiny)
// critical section via NullcountGuard, so a thread is never parked at the barrier
// holding it. The in-barrier reclaim runs on the barrier (orchestrator) thread,
// which its own barrier never signals, so it locks plainly.
static ck::mutex nullcount_lock;

struct NullcountGuard {
  sigset_t old;
  NullcountGuard() {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    nullcount_lock.lock();
  }
  ~NullcountGuard() {
    nullcount_lock.unlock();
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
  }
};

static inline void nullcount_update(void* ptr, bool add) {
  NullcountGuard g;
  if (add) nullcount_map.set(ptr);
  else nullcount_map.remove(ptr);
}

// Defined in halloc.cpp -- the calling thread's (raw) thread cache.
extern alaska::ThreadCache *get_tc_r(void);


extern "C" {

/*
  HACK: this is not the most sophisticated way to prevent recursion, but it works for now... 
  
  For some reason, the refcount operations themselves can trigger refcount operations (e.g. if we need to read/write a handle's metadata).
  This shouldn't really happen as alaska functions are supposed to be escaped
*/
static thread_local bool in_refcount_operation = false;

/**
 * alaska_inc_refcount - Increment the reference count of a handle
 * 
 * This function is called by the compiler when a handle is written to heap memory.
 * It checks if the pointer is actually a handle and increments its refcount.
 * 
 * @param ptr - The potential handle whose refcount should be incremented
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_inc_refcount(void *ptr) {
  // print what is at the ptr in hex
  
  if (ptr == nullptr) return; 
  if (in_refcount_operation) return;
  
  in_refcount_operation = true;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do
    in_refcount_operation = false;
    return;
  }

  // alaska::printf("Incrementing refcount of mapping  %p from %lu\n", mapping, mapping->get_refcount());
  // Increment the refcount using the mapping's method
  auto new_count = mapping->inc_refcount();
  if(new_count == 1) {
    nullcount_update(ptr, /*add=*/false);
  }

  in_refcount_operation = false;
}

/**
 * alaska_dec_refcount - Decrement the reference count of a handle
 * 
 * This function is called by the compiler when a handle is being overwritten.
 * It checks if the pointer is actually a handle and decrements its refcount.
 * If the refcount reaches zero, the handle could potentially be freed.
 * 
 * @param ptr - The potential handle whose refcount should be decremented
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_dec_refcount(void *ptr) {
  if (ptr == nullptr) return;
  
  // Prevent infinite recursion if this function itself triggers refcount operations
  if (in_refcount_operation) return;
  in_refcount_operation = true;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do
    in_refcount_operation = false;
    return;
  }

  // Decrement the refcount using the mapping's method
  uint64_t new_count = mapping->dec_refcount();
  
  // TODO: If refcount reaches 0, we could potentially free the handle
  // For now, we just track the refcount. The actual freeing policy
  // should attach here
  if (new_count == 0) {
   nullcount_update(ptr, /*add=*/true);
  } else {
   // The refcount dropped but is still non-zero. This is the only situation in
   // which `mapping` can become the root of a garbage *cycle*, so hand it to
   // Anchorage's cycle collector as a candidate root (Bacon & Rajan "purple").
   alaska::Runtime::get().cycle_collector.register_candidate(mapping);
  }

  in_refcount_operation = false;
}

/**
 * alaska_get_refcount - Get the current reference count of a handle
 * 
 * This function allows users to query the reference count of a handle.
 * It checks if the pointer is actually a handle and returns its refcount.
 * 
 * @param ptr - The potential handle whose refcount should be retrieved
 * @return The refcount if ptr is a valid handle, or 0 if ptr is NULL or not a handle
 */
unsigned long alaska_get_refcount(void *ptr) {
  if (ptr == nullptr) return 0;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, return 0
    return 0;
  }
  
  // Get and return the refcount using the mapping's method
  return (unsigned long)mapping->get_refcount();
}

int alaska_nullcount_map_size(){
  NullcountGuard g;
  return nullcount_map.size();
}

void alaska_nullcount_map_foreach(void (*fn)(void* ptr)) {
  NullcountGuard g;
  for (auto it = nullcount_map.begin(); it != nullcount_map.end(); ++it) {
    fn(*it);
  }
}

// Copy the current set of zero-refcount handles into `out` (capacity `cap`) under
// the lock, so the caller iterates a private snapshot instead of the live map
// (which mutators rehash concurrently). Returns the *total* number of entries; if
// that exceeds `cap` the caller should grow `out` and call again.
size_t alaska_nullcount_snapshot(void **out, size_t cap) {
  NullcountGuard g;
  size_t n = 0;
  size_t total = 0;
  for (auto it = nullcount_map.begin(); it != nullcount_map.end(); ++it) {
    if (n < cap) out[n++] = *it;
    total++;
  }
  return total;
}

// Drop a handle from the zero-refcount set.
void alaska_nullcount_forget(void *ptr) {
  NullcountGuard g;
  nullcount_map.remove(ptr);
}

inline int alaska_is_handle(void *ptr){
  return alaska::Mapping::is_handle(ptr);
}

/**
 * alaska_collect_cycles - Run one cycle collection inside Anchorage.
 *
 * Stops the world (via Anchorage's barrier) and runs synchronous trial-deletion
 * cycle collection over the buffered candidate roots, reclaiming any handles
 * that are only kept alive by reference cycles. Returns the number of handles
 * reclaimed. Note the barrier has a minimum interval, so back-to-back calls may
 * return 0 simply because no barrier was taken.
 */
unsigned long alaska_collect_cycles(void) {
  auto &rt = alaska::Runtime::get();
  // Make sure this thread has a thread cache *before* entering the barrier:
  // creating one needs locks the barrier already holds.
  auto *tc = get_tc_r();
  unsigned long reclaimed = 0;
  rt.with_barrier([&]() { reclaimed = rt.cycle_collector.collect(*tc); });
  return reclaimed;
}

// Number of candidate cycle roots currently buffered.
unsigned long alaska_cycle_candidate_count(void) {
  return alaska::Runtime::get().cycle_collector.candidate_count();
}

// Total number of handles reclaimed by the cycle collector so far.
unsigned long alaska_cycles_collected(void) {
  return alaska::Runtime::get().cycle_collector.total_collected();
}
}  // extern "C"


namespace alaska {

  // Stackscan reclamation, run from barrier_thread_func INSIDE with_barrier (world
  // stopped). Free every zero-refcount handle that the per-thread conservative scan
  // did NOT mark present -- i.e. that is not on any thread's stack. With the world
  // stopped the present set is a perfect snapshot, so a single pass is provably
  // correct (no two-cycle/hazard). Frees via tc.hfree, exactly like
  // CycleCollector::reclaim. Runs on the barrier (orchestrator) thread, which its
  // own barrier never signals, so it locks nullcount_lock plainly; mutators are all
  // parked at the barrier and -- thanks to NullcountGuard -- none holds the lock.
  // Collect-then-free avoids mutating nullcount_map mid-iteration; hfree (which does
  // not touch nullcount_map) runs after the lock is dropped.
  size_t reclaim_dead_handles(alaska::ThreadCache &tc) {
    size_t candidates = 0;
    ck::vec<void *> to_free;

    // Try (do NOT block) to take nullcount_lock. The world is stopped, so the
    // only possible holder is a mutator the barrier parked mid-nullcount-update
    // -- in which case the map may even be mid-rehash and inconsistent. Blocking
    // here would deadlock: that thread cannot release the lock until we end the
    // barrier, which we cannot do until this callback returns. Skipping the cycle
    // is both deadlock-free and correct -- we reclaim on the next barrier, when
    // the lock is free and the map is consistent. (Masking SIGUSR2 around the
    // mutator's critical section does NOT prevent this: under liballocs/systrap a
    // syscall inside the section -- e.g. the rehash mmap -- still lets the pending
    // barrier signal through and parks the thread while it holds the lock.)
    if (nullcount_lock.try_lock() != 0) {
      if (getenv("RECLAIM_DEBUG"))
        fprintf(stderr, "[reclaim] nullcount_lock contended (parked mutator); skipping cycle\n");
      return 0;
    }
    for (auto it = nullcount_map.begin(); it != nullcount_map.end(); ++it) {
      void *h = *it;
      candidates++;
      auto *m = alaska::Mapping::from_handle_safe(h);
      if (m == nullptr) continue;
      if (m->is_free()) continue;
      if (m->get_refcount() != 0) continue;        // re-published (defensive)
      if (alaska::gc::present_test(m)) continue;    // on some thread's stack
      to_free.push(h);
    }
    for (auto *h : to_free) nullcount_map.remove(h);
    nullcount_lock.unlock();

    for (auto *h : to_free) tc.hfree(h);
    if (getenv("RECLAIM_DEBUG")) {
      fprintf(stderr, "[reclaim] candidates=%zu present_marks=%lu freed=%zu\n",
              candidates, alaska::gc::g_mark_count, (size_t)to_free.size());
    }
    return (size_t)to_free.size();
  }

}  // namespace alaska
