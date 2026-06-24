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
#include <alaska/EventCounters.hpp>
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
  // Keep the per-Mapping on-nullcount hint (Mapping::kOnNullcountBit) in lockstep
  // with map membership, so alaska_inc_refcount can decide whether it even needs to
  // take this guard without consulting the map. Maintained under the lock so the
  // bit and the map never disagree for a handle that is genuinely on the list.
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (add) {
    nullcount_map.set(ptr);
    if (m) m->set_on_nullcount(true);
  } else {
    nullcount_map.remove(ptr);
    if (m) m->set_on_nullcount(false);
  }
}

// Defined in halloc.cpp -- the calling thread's (raw) thread cache.
extern alaska::ThreadCache *get_tc_r(void);

// The inc/dec barrier fast paths now live in core/translate.cpp (so they inline at
// each store site instead of being a cross-library call); declare them for the few
// in-library callers below (alaska_inc_handles_in_range / alaska_hfree_dec_children).
extern "C" void alaska_inc_refcount(void *ptr);
extern "C" void alaska_dec_refcount(void *ptr);


extern "C" {

/*
  HACK: this is not the most sophisticated way to prevent recursion, but it works for now... 
  
  For some reason, the refcount operations themselves can trigger refcount operations (e.g. if we need to read/write a handle's metadata).
  This shouldn't really happen as alaska functions are supposed to be escaped
*/
static thread_local bool in_refcount_operation = false;

/**
 * alaska_inc_refcount_nullcount - Slow path of the increment barrier.
 *
 * The FAST path (handle check + the whole-word CAS via Mapping::inc_refcount) is
 * inlined at every store site from core/translate.cpp, so it never calls into this
 * library. It only calls here in the rare case where an incremented handle reaches
 * refcount 1 while still on the zero-refcount nullcount list -- i.e. it was dec'd to
 * 0 earlier and is now resurrected -- in which case it must be dropped from the list
 * so the GC reclaim does not free a now-live handle.
 *
 * The recursion guard prevents a re-entrant refcount op (e.g. an allocator
 * birth-bump triggered while the nullcount map rehashes) from recursively taking
 * nullcount_lock and deadlocking. (The fast path never re-enters -- it is a pure
 * CAS -- so the guard only needs to wrap this lock-taking slow path.)
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_inc_refcount_nullcount(void *ptr) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  if (in_refcount_operation) return;
  in_refcount_operation = true;
  nullcount_update(ptr, /*add=*/false);
  in_refcount_operation = false;
#endif
}

/**
 * alaska_dec_refcount_slow - Slow path of the decrement barrier.
 *
 * The FAST path (handle check + the whole-word CAS via Mapping::dec_refcount) is
 * inlined at every overwrite site from core/translate.cpp, so it never calls into
 * this library -- in particular the dominant case of decrementing a freshly-zeroed
 * (null) slot before a first store returns inline. This routes a just-decremented
 * handle to the GC: onto the zero-refcount nullcount list (count hit 0) or, if
 * still positive, into the cycle collector's candidate set. Only built/reached in
 * GC builds; a plain refcount build just tracks counts and never reclaims.
 *
 * The recursion guard prevents a re-entrant refcount op (e.g. an allocator bump
 * while the nullcount map rehashes) from recursively taking nullcount_lock and
 * deadlocking. (The fast path never re-enters -- it is a pure CAS.)
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_dec_refcount_slow(void *ptr, int new_count) {
#if ALASKA_ENABLE_CYCLE_COLLECTION
  if (new_count == 0) {
    // Record as a zero-refcount handle for the stackscan reclaim to consider.
    nullcount_update(ptr, /*add=*/true);
  } else {
    // The refcount dropped but is still non-zero -- the only situation in which the
    // handle can become the root of a garbage *cycle*, so hand it to the cycle
    // collector as a candidate root (Bacon & Rajan "purple").
    auto *m = alaska::Mapping::from_handle_safe(ptr);
    if (m) alaska::Runtime::get().cycle_collector.register_candidate(m);
  }
  in_refcount_operation = false;
#endif
}

/**
 * alaska_hfree_dec_children - Drop the references an aggregate held, on free.
 *
 * When an object is freed its outgoing handle references die too, so decrement
 * each contained handle's reference count. This is what finally lets refcounts
 * return to zero for handles only reachable through heap containers: without it a
 * stored-once handle is inc'd (on the heap write) but never dec'd, so its count
 * never reaches 0 and refcount/GC reclamation never triggers -- the dominant case
 * in practice.
 *
 * The scan is conservative -- the same word-walk the cycle collector's
 * visit_children uses -- so a word that merely looks like a live handle is treated
 * as one. UNLIKE the cycle collector (which only *trial* decrements and then
 * restores live nodes), this decrement is PERMANENT: a false-positive word could
 * drive a live object's refcount to 0 and have it reclaimed prematurely. That is
 * the inherent hazard of conservative refcount-on-free. Disable at runtime with
 * ALASKA_NO_FREE_DEC. Called from alaska_hfree_now before the backing memory is
 * recycled, while the object is still readable.
 */
// True only for a word that decodes to a real, LIVE handle -- the same validation
// the cycle collector's is_collectable uses. The conservative heap scans below
// feed arbitrary memory words here, so the decoded mapping MUST be bounds-checked
// before it is touched: Mapping::from_handle_safe returns a structurally-decoded
// pointer for ANY value whose top (handle) bit is set, which for junk data is a
// wild pointer that inc/dec_refcount would dereference and crash on. valid_handle
// is pure pointer arithmetic (no deref); only after it passes is reading is_free
// safe. (Mirrors CycleCollector::is_collectable.)
static inline bool word_is_live_handle(void *p) {
  auto *m = alaska::Mapping::from_handle_safe(p);
  if (m == nullptr) return false;
  if (!alaska::Runtime::get().handle_table.valid_handle(m)) return false;
  if (m->is_free()) return false;
  return true;
}

void alaska_hfree_dec_children(void *ptr) {
  // OPT-IN (default OFF): conservative dec-on-free is UNSOUND for programs that
  // manage memory explicitly and use self-referential structures. Freeing a node
  // decrements the handles it holds, but a doubly-linked node's forward/back point
  // at still-live neighbours; over-decrementing them lets the GC reclaim live
  // nodes mid-traversal, corrupting the structure (observed as a hang in Olden
  // `health`). Enable only for experiments with ALASKA_FREE_DEC=1.
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_FREE_DEC") != nullptr) ? 1 : 0;
  if (!enabled) return;
  if (ptr == nullptr || in_refcount_operation) return;

  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr || m->is_free()) return;

  auto *tc = get_tc_r();
  if (tc == nullptr) return;
  size_t size = tc->get_size(ptr);
  if (size < sizeof(void *)) return;

  void **words = (void **)m->get_pointer();
  if (words == nullptr) return;

  // Only decrement words that are validated live handles -- a raw word that merely
  // has the handle bit set is NOT safe to hand to dec_refcount (see above).
  size_t n = size / sizeof(void *);
  for (size_t i = 0; i < n; i++) {
    if (words[i] != nullptr && word_is_live_handle(words[i])) alaska_dec_refcount(words[i]);
  }
}

/**
 * alaska_inc_handles_in_range - Inc the handles a byte copy deposited.
 *
 * The store barrier (RefcountInc) only increments on pointer-*typed* stores, so a
 * memcpy/memmove of handle-containing memory creates new heap references to those
 * handles WITHOUT incrementing them. They would then be decremented on free
 * (alaska_hfree_dec_children) with no matching inc -- an underflow that could free
 * a live object. The compiler instruments every byte copy with a call to this
 * function over the destination range, keeping inc/dec balanced.
 *
 * Conservative word scan, exactly mirroring alaska_hfree_dec_children: a word that
 * decodes to a live handle is inc'd (alaska_inc_refcount ignores everything else).
 * `base` is the raw destination pointer, `bytes` the copy length.
 */
void alaska_inc_handles_in_range(void *base, size_t bytes) {
  // OPT-IN (default OFF): same conservative-scan class as alaska_hfree_dec_children
  // (its inc-side partner). It is only meaningful when dec-on-free is also enabled,
  // and it over-increments handle-looking data, so it stays off unless requested
  // with ALASKA_COPY_INC=1.
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_COPY_INC") != nullptr) ? 1 : 0;
  if (!enabled) return;
  if (base == nullptr || bytes < sizeof(void *)) return;
  if (in_refcount_operation) return;
  void **words = (void **)base;
  // Only increment validated live handles. This scans arbitrary copied bytes, so
  // a raw word with the handle bit set is NOT safe to hand to inc_refcount without
  // the bounds check in word_is_live_handle (see there).
  size_t n = bytes / sizeof(void *);
  for (size_t i = 0; i < n; i++) {
    if (words[i] != nullptr && word_is_live_handle(words[i])) alaska_inc_refcount(words[i]);
  }
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

// Record a handle as zero-refcount. The cycle collector calls this (weakly) to
// hand a proven garbage-cycle member to the stackscan reclaim path instead of
// freeing it directly: trial deletion has already driven the handle's refcount to
// 0, but only reclaim_dead_handles -- after the conservative stack scan -- may
// actually free it. Both run in the same barrier (see barrier_thread_func), so a
// genuinely dead member is reclaimed this same cycle.
void alaska_nullcount_add(void *ptr) {
  nullcount_update(ptr, /*add=*/true);
}

inline int alaska_is_handle(void *ptr){
  return alaska::Mapping::is_handle(ptr);
}

#if ALASKA_ENABLE_CYCLE_COLLECTION
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
#endif  // ALASKA_ENABLE_CYCLE_COLLECTION
}  // extern "C"


// liballocs lifetime-policy bridges (weak: Alaska runs standalone without them).
// __liballocs_alaska_manual_pinned reports whether the manual lifetime policy is
// attached to a backing object -- if so the GC must NOT reclaim it (the manual
// pin overrides the GC). __liballocs_notify_alaska_free drops liballocs' side-table
// type/site record for a base; reclaim calls it because it frees via tc.hfree,
// bypassing the public hfree that would otherwise notify. See
// contrib/liballocs/src/allocators/alaska.c.
extern "C" int  __liballocs_alaska_manual_pinned(void *base) __attribute__((weak));
extern "C" void __liballocs_notify_alaska_free(void *base)   __attribute__((weak));

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
      // Manual lifetime policy overrides the GC: a pinned handle is kept alive
      // even at refcount 0 and stack-unreachable. It stays in nullcount_map and is
      // reconsidered on later barriers, becoming reclaimable once unpinned (hfree).
      if (&__liballocs_alaska_manual_pinned &&
          __liballocs_alaska_manual_pinned(m->get_pointer())) continue;
      to_free.push(h);
    }
    for (auto *h : to_free) nullcount_map.remove(h);
    nullcount_lock.unlock();

    for (auto *h : to_free) {
      if (&__liballocs_notify_alaska_free) {
        auto *m = alaska::Mapping::from_handle_safe(h);
        if (m) __liballocs_notify_alaska_free(m->get_pointer());
        hfree(h); // we might hijack this from liballocs
      } else {
        // hfree(h);
        ::alaska_hfree_dec_children(h);
        tc.hfree(h); // optimisation in case liballocs is 
      }
      
      
    }
    if (getenv("RECLAIM_DEBUG")) {
      fprintf(stderr, "[reclaim] candidates=%zu present_marks=%lu freed=%zu\n",
              candidates, alaska::gc::g_mark_count, (size_t)to_free.size());
    }
#if ALASKA_ENABLE_EVENT_COUNTERS
    alaska::events::gc_free_event((uint64_t)to_free.size());
#endif
    return (size_t)to_free.size();
  }

}  // namespace alaska
