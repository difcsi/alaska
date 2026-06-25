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

namespace alaska::gc { extern unsigned long g_mark_count; }

// Handles whose reference count has reached zero are tracked in a lock-free side
// bitmap -- one bit per handle-table slot (see alaska::gc::nullcount_bm_*). dec->0
// sets the bit, inc->1 / free clears it, and the in-barrier reclaim scans it.
//
// This replaced an earlier nullcount_map hashmap guarded by a SIGUSR2-masked lock.
// An A/B showed the bitmap an order of magnitude cheaper on the refcount hot path --
// a single atomic OR vs. a lock plus two pthread_sigmask syscalls per dec->0 -- with
// byte-for-byte identical reclamation, so the hashmap was removed. Because set/clear
// are single atomic ops the bitmap can never be read
// torn (unlike a hashmap mid-rehash), so a mutator the barrier parks mid-update is
// harmless and the reclaim scan needs no lock and never has to skip a cycle.

static inline void nullcount_update(void* ptr, bool add) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return;
  // Keep the per-Mapping on-nullcount hint (Mapping::kOnNullcountBit) in lockstep
  // with the bitmap, so the inlined inc fast path (translate.cpp) knows whether a
  // resurrected handle needs the slow clear without consulting the bitmap.
  if (add) {
    alaska::gc::nullcount_bm_set(m);
    m->set_on_nullcount(true);
  } else {
    alaska::gc::nullcount_bm_clear(m);
    m->set_on_nullcount(false);
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
 * birth-bump triggered while updating GC metadata) from recursively re-entering
 * this slow path. (The fast path never re-enters -- it is a pure CAS -- so the
 * guard only needs to wrap this slow path.)
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
 * while updating GC metadata) from recursively re-entering this slow path. (The
 * fast path never re-enters -- it is a pure CAS.)
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
 * recycled, while the object is still readable -- OR (the default) from the barrier
 * thread's deferred-free drain (ThreadCache::drain_deferred), which keeps the object
 * alive in a per-thread queue so this scan runs off the freeing thread's hot path (see
 * ThreadCache::defer_free and barrier_thread_func; opt out with ALASKA_NO_FREE_DEC_DEFER).
 */
// True only for a word that decodes to a real, LIVE handle -- the same validation
// the cycle collector's is_collectable uses. The conservative heap scans below feed
// arbitrary memory words here, so the decoded mapping MUST be bounds-checked before it
// is touched: Mapping::from_handle_safe returns a structurally-decoded pointer for ANY
// value whose top (handle) bit is set, which for junk data is a wild pointer that
// inc/dec_refcount would dereference and crash on. HandleTable::is_live_handle does the
// bounds-check (pure pointer arithmetic, no deref) before any field is read.
//
// It uses is_live_handle, NOT valid_handle() + !is_free(): a slot sitting on the handle
// allocator's free list keeps its invl bit clear (its word is a raw next-link), so
// is_free() reports it live and the inc/dec below would CAS-corrupt that link. See
// HandleTable::is_live_handle. (Mirrors CycleCollector::is_collectable.)
static inline bool word_is_live_handle(void *p) {
  auto *m = alaska::Mapping::from_handle_safe(p);
  return alaska::Runtime::get().handle_table.is_live_handle(m);
}

void alaska_hfree_dec_children(void *ptr) {
  // DEFAULT ON (opt out with ALASKA_NO_FREE_DEC=1): dropping an aggregate's outgoing
  // handle references on free is required for refcounts to actually reach 0 and be
  // reclaimed (a stored-once handle is otherwise inc'd but never dec'd). It is paired
  // with copy-inc (alaska_inc_handles_in_range, also default on) so memcpy'd references
  // stay balanced. CAVEAT: conservative dec-on-free is UNSOUND for programs that manage
  // memory explicitly with self-referential structures -- an undercounted reference (a
  // missed inc, a conservative false positive) can drive a still-referenced node to 0
  // and have the GC reclaim it (observed as a hang in Olden `health`). Disable there
  // with ALASKA_NO_FREE_DEC=1.
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_NO_FREE_DEC") != nullptr) ? 0 : 1;
  if (!enabled) return;
  if (ptr == nullptr || in_refcount_operation) return;

  auto *tc = get_tc_r();
  if (tc == nullptr) return;

  // Resolve the object's backing base. For a SIZED handle the base is the mapping's
  // backing pointer; for a HUGE object there is no mapping and `ptr` already IS the raw
  // backing pointer. Handling the huge case matters for correctness: the inc barrier
  // fires on handle stores into a huge object too, so skipping its fields here would
  // leak those children's refcounts (they would never reach 0 / be reclaimed).
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m != nullptr && m->is_free()) return;
  void **words = (m != nullptr) ? (void **)m->get_pointer() : (void **)ptr;
  if (words == nullptr) return;

  size_t size = tc->get_size(ptr);  // get_size resolves huge sizes via the huge allocator
  if (size < sizeof(void *)) return;

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
  // DEFAULT ON (opt out with ALASKA_NO_COPY_INC=1): inc-side partner of dec-on-free
  // (alaska_hfree_dec_children). A byte copy of handle-containing memory creates new heap
  // references without firing the store barrier; this re-balances them so the matching
  // dec-on-free does not underflow and free a live object. Kept in lockstep with
  // dec-on-free's default so the two stay balanced.
  static int enabled = -1;
  if (enabled < 0) enabled = (getenv("ALASKA_NO_COPY_INC") != nullptr) ? 0 : 1;
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
  return (int)alaska::gc::nullcount_bm_count();
}

void alaska_nullcount_map_foreach(void (*fn)(void* ptr)) {
  ck::vec<alaska::Mapping *> cands;
  alaska::gc::nullcount_bm_collect(cands);
  for (auto *m : cands) fn(m->to_handle());
}

// Copy the current set of zero-refcount handles into `out` (capacity `cap`).
// Returns the *total* number of entries; if that exceeds `cap` the caller should
// grow `out` and call again.
size_t alaska_nullcount_snapshot(void **out, size_t cap) {
  ck::vec<alaska::Mapping *> cands;
  alaska::gc::nullcount_bm_collect(cands);
  size_t n = 0;
  for (auto *m : cands)
    if (n < cap) out[n++] = m->to_handle();
  return cands.size();
}

// Drop a handle from the zero-refcount set. Called by hfree when freeing a handle that
// reached refcount 0: without this the dead handle lingers in the bitmap, and once its
// mapping slot is recycled by a later allocation the reused (live) handle inherits a
// stale "collectable" bit and the stackscan reclaim frees it out from under the mutator.
// Clears the per-Mapping hint too, so the bit and the hint stay in lockstep (see
// nullcount_update).
void alaska_nullcount_forget(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m) {
    alaska::gc::nullcount_bm_clear(m);
    m->set_on_nullcount(false);
  }
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
  // CycleCollector::reclaim.
  //
  // The zero-refcount set is the lock-free bitmap (alaska::gc::nullcount_bm_*),
  // mutated only by atomic OR/AND. A mutator the barrier parked mid-update therefore
  // cannot leave it torn (unlike a hashmap mid-rehash), and the world is stopped so
  // no set races this scan -- so unlike the old hashmap path this needs no lock and
  // never has to skip a cycle.
  size_t reclaim_dead_handles(alaska::ThreadCache &tc) {
    size_t candidates = 0;
    ck::vec<void *> to_free;

    ck::vec<alaska::Mapping *> cands;
    alaska::gc::nullcount_bm_collect(cands);
    for (auto *m : cands) {
      candidates++;
      if (m->is_free()) continue;
      if (m->get_refcount() != 0) continue;       // re-published (defensive)
      if (alaska::gc::present_test(m)) continue;   // on some thread's stack
      // Manual lifetime policy overrides the GC: a pinned handle is kept alive even
      // at refcount 0 and stack-unreachable. Its bit stays set and it is reconsidered
      // on later barriers, becoming reclaimable once unpinned (hfree).
      if (&__liballocs_alaska_manual_pinned &&
          __liballocs_alaska_manual_pinned(m->get_pointer())) continue;
      to_free.push(m->to_handle());
    }
    // Clear the reclaimed handles' bits AND the per-Mapping hint, keeping the two in
    // lockstep (see nullcount_update). Clearing the hint also means the hfree below
    // sees is_on_nullcount() == false and skips its own redundant nullcount_forget.
    for (auto *h : to_free) {
      auto *m = alaska::Mapping::from_handle_safe(h);
      if (m) {
        alaska::gc::nullcount_bm_clear(m);
        m->set_on_nullcount(false);
      }
    }

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
