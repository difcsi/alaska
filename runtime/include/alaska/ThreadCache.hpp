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

#include <alaska/Heap.hpp>
#include <alaska/HeapPage.hpp>
#include <alaska/HandleTable.hpp>
#include <alaska/LocalityPage.hpp>
#include <alaska/alaska.hpp>
#include <alaska/Localizer.hpp>
#include "ck/lock.h"
#include <ck/vec.h>

namespace alaska {

  struct Runtime;

  // A ThreadCache is the class which manages the thread-private
  // allocations out of a shared heap. The core runtime itself does
  // *not* manage where thread caches are used. It is assumed
  // something else manages storing a pointer to a ThreadCache in some
  // thread-local variable
  class ThreadCache final : public alaska::InternalHeapAllocated {
   public:
    ThreadCache(int id, alaska::Runtime &rt);

    void *halloc(size_t size, bool zero = false);
    void *hrealloc(void *handle, size_t new_size);
    void hfree(void *handle);

#if ALASKA_ENABLE_REFCOUNT
    // Deferred dec-on-free (the default; opt out with ALASKA_NO_FREE_DEC_DEFER): instead of
    // running the expensive alaska_hfree_dec_children scan + backing free on the freeing
    // thread's hot path, hfree enqueues the handle via defer_free and the barrier thread
    // drains the queue via drain_deferred (world stopped). BOTH methods assume the caller
    // already holds this->lock -- the get_tc() LockedThreadCache on the mutator enqueue path,
    // and lock_all_thread_caches() on the barrier drain path.
    void defer_free(void *handle);
    void drain_deferred(void);
    // Release the handle slots the deferred-drain quarantine withheld one barrier epoch
    // ago, and roll the current epoch's quarantine forward. Called once per barrier AFTER
    // every thread's deferred queue has drained (see hfree_impl / quarantine_rotate).
    void quarantine_rotate(void);
    // Flush the Perceus reuse-cache stash (free its backing + return its slot). Called once per
    // barrier before the heap-walking passes so no stashed backing sits in reuse limbo during
    // compaction (see hfree_impl reuse path / barrier_thread_func). Public so the barrier can
    // call it; also used internally on page swap-out and tc teardown. Caller holds this->lock.
    void flush_reuse_cache(void);

    // --- Deferred reference-count increment (Levanoni-Petrank) --------------------
    // The hot-path increment barrier (alaska_inc_refcount -> alaska_defer_inc) appends the
    // target Mapping here instead of doing the scattered handle-table refcount CAS; the
    // barrier applies the whole log in one address-sorted, coalesced sweep (drain_inc),
    // turning ~N scattered table misses into a streaming pass. Runtime-gated by
    // ALASKA_DEFER_RC. defer_inc is the ONLY ThreadCache method that does NOT take
    // this->lock: a single-producer append into a preallocated buffer, made safe against
    // the barrier by the stop-the-world rendezvous (its sole consumer, drain_inc, runs
    // world-stopped and reads inc_log_head only after every mutator is parked). It returns
    // false (without appending) when the log is full/unallocated so the caller applies that
    // one increment eagerly -- correctness never depends on the buffer having room.
    // drain_inc DOES require this->lock (the barrier holds it via lock_all_thread_caches,
    // or the freeing thread holds its own before an inline dec-on-free bypass).
    bool defer_inc(alaska::Mapping *m) {
      uint32_t h = inc_log_head;
      if (unlikely(inc_log == nullptr || h >= inc_log_cap)) {
#if ALASKA_ENABLE_EVENT_COUNTERS
        alaska::events::rc_overflow_event();
#endif
        return false;
      }
      inc_log[h] = m;          // (1) write the slot, THEN
      inc_log_head = h + 1;    // (2) publish it. Plain stores: the stop-the-world barrier is
                               // the only fence to the (sole) consumer drain_inc. If a barrier
                               // signal lands between (1) and (2) this entry is just applied
                               // next epoch; its target is register-live at the store, so the
                               // conservative present-scan pins it this epoch (INV-LIVE).
#if ALASKA_ENABLE_EVENT_COUNTERS
      alaska::events::rc_deferred_event();
#endif
      return true;
    }
    void drain_inc(void);
    ~ThreadCache(void);
#endif

    int get_id(void) const { return this->id; }
    size_t get_size(void *handle);


    bool localize(alaska::Mapping &m, uint64_t epoch);
    bool localize(void *handle, uint64_t epoch);


   protected:
    friend class LockedThreadCache;
    friend alaska::Runtime;
    friend alaska::Localizer;

    ck::mutex lock;

    // Allocate backing data for a handle, but don't assign it yet.
    void *allocate_backing_data(const alaska::Mapping &m, size_t size);

    // Free an allocation behind a handle, but not the handle
    void free_allocation(const alaska::Mapping &m);

    // Shared body of hfree. When `quarantine_slot` is true (the legacy deferred-drain path) the
    // backing is freed and the pointer cleared, but the mapping slot is withheld from the
    // allocatable pool for one barrier epoch instead of being put back immediately.
    // `allow_reuse` gates the synchronous Perceus reuse-cache stash: it MUST be false on any
    // deferred-drain path (a stashed entry sits in limbo -- backing off the freelist, mapping
    // not reset -- which the in-barrier compaction that runs right after the drain then
    // mishandles into a double free). The synchronous hfree() leaves it true.
    void hfree_impl(void *handle, bool quarantine_slot, bool allow_reuse = true);

    // Allocate a new handle table mapping
    alaska::Mapping *new_mapping(void);
    // Swap to a new sized page owned by this thread cache
    alaska::SizedPage *new_sized_page(int cls);

#if ALASKA_ENABLE_REFCOUNT
    // Perceus-flavored reuse cache (needs the refcount; inert on the default deferred-free
    // path). One slot per size class: when a uniquely-owned handle (refcount <= 1) is freed
    // on the SYNCHRONOUS path, hfree_impl stashes its still-live (mapping, backing) pair here
    // instead of returning the backing to the freelist and the slot to the handle table.
    // The next same-size halloc re-hands the same handle + backing, skipping the whole
    // round-trip (the page Header still points at the mapping). `page` is recorded so the
    // alloc path can verify the page is still this tc's current page for the class before
    // reusing (a swapped-out page invalidates the entry).
    struct ReuseEntry {
      alaska::Mapping *m = nullptr;
      void *ptr = nullptr;
      alaska::SizedPage *page = nullptr;
    };
    ReuseEntry reuse_cache[alaska::num_size_classes];

    // Drop a cached entry back through the normal free path (free backing + return slot).
    void flush_reuse_class(int cls);
    // flush_reuse_cache() is declared public above (the barrier calls it).
#endif
    // Swap to a new locality page owned by this thread cache
    alaska::LocalityPage *new_locality_page(size_t required_size);

    // Just an id for this thread cache assigned by the runtime upon creation. It's mostly
    // meaningless, meant for debugging.
    int id;
    // A reference to the global runtime. This is here mainly to gain
    // access to the HandleTable and the Heap.
    alaska::Runtime &runtime;

    // A pointer to the current slab of the handle table that this thread
    // cache is allocating from.
    alaska::HandleSlab *handle_slab;

    // Each thread cache has a private heap page for each size class
    // it might allocate from. When a size class fills up, it is
    // returned to the global heap and another one is allocated.
    alaska::SizedPage *size_classes[alaska::num_size_classes] = {nullptr};
    // Each thread cache also has a private "Locality Page", which
    // objects can be relocated to according to some external
    // policy. This page is special because it can contain many
    // objects of many different sizes.
    alaska::LocalityPage *locality_page = nullptr;

#if ALASKA_ENABLE_REFCOUNT
    // Handles awaiting deferred dec-on-free, with the running total of their backing
    // bytes for the byte-cap backpressure check. Guarded by `lock` (see defer_free).
    ck::vec<void *> deferred_frees;
    size_t deferred_bytes = 0;

    // Handle-slot quarantine for the deferred-drain path. A slot whose backing was freed
    // during drain_deferred is parked here (NOT returned to the allocatable pool) until a
    // later barrier, so a not-yet-drained parent's stale child word cannot alias a reused
    // slot and have dec-on-free corrupt the new occupant (the binarytrees UAF). Two
    // generations: `cur` collects this epoch's withheld slots; `prev` holds last epoch's
    // and is released at the next quarantine_rotate.
    ck::vec<alaska::Mapping *> quarantine_cur;
    ck::vec<alaska::Mapping *> quarantine_prev;

    // Record-at-enqueue drain strategy (ALASKA_FREE_DEC_RECORD=1; A/B alternative to the
    // legacy reread+quarantine path). The live child handles to decrement, captured by
    // defer_free while the freed parent is still coherent (before any sibling's slot can be
    // reused) and replayed by drain_deferred. Decoupling the is_live_handle DECISION (made
    // here, at enqueue) from the dec ARITHMETIC (deferred to the barrier) is what makes the
    // drain sound without a slot quarantine -- see defer_free / drain_deferred. Guarded by
    // `lock`, like deferred_frees.
    ck::vec<void *> deferred_child_decs;

    // Deferred-RC increment log (Levanoni-Petrank; see defer_inc / drain_inc). A fixed,
    // preallocated single-producer buffer of target mappings awaiting a batched refcount
    // add. NOT a ck::vec: the hot-path append must be malloc-free and async-signal-safe (a
    // ck::vec push can realloc), and ck::vec::sort() truncates Mapping* through int.
    // Allocated once in the ctor; if that allocation fails inc_log stays null and defer_inc
    // always reports overflow (a clean degrade to eager increments).
    // Capacity (entries) of the preallocated log, set once from ALASKA_INC_LOG_CAP at ctor
    // (default 4,194,304 = 32 MiB/thread). A bigger log lets a fast-appending deferred mutator
    // accumulate a DENSER per-epoch batch before the barrier drains it -- which both avoids the
    // eager-fallback overflow AND gives the address sort more equal-address / same-line runs to
    // coalesce. Tune it (no rebuild) until rc_overflow hits 0.
    size_t inc_log_cap = 0;
    alaska::Mapping **inc_log = nullptr;          // the log; entries [0, inc_log_head) pending
    uint32_t inc_log_head = 0;                    // append/commit cursor
#endif

   public:
    // Each thread cache has a localizer, which can be fed with
    // "localization data" to improve object locality
    alaska::Localizer localizer;
  };



  class LockedThreadCache final {
   public:
    LockedThreadCache(ThreadCache &tc)
        : tc(tc) {
      tc.lock.lock();
    }


    ~LockedThreadCache(void) { tc.lock.unlock(); }

    // Delete copy constructor and copy assignment operator
    LockedThreadCache(const LockedThreadCache &) = delete;
    LockedThreadCache &operator=(const LockedThreadCache &) = delete;

    // Delete move constructor and move assignment operator
    LockedThreadCache(LockedThreadCache &&) = delete;
    LockedThreadCache &operator=(LockedThreadCache &&) = delete;




    ThreadCache &operator*(void) { return tc; }
    ThreadCache *operator->(void) { return &tc; }

   private:
    ThreadCache &tc;
  };



}  // namespace alaska
