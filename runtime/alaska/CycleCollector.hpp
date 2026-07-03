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

#include <alaska/alaska.hpp>
#include <ck/vec.h>
#include <ck/set.h>
#include <ck/map.h>
#include <ck/lock.h>
#include <ck/func.h>

namespace alaska {

  // fwd decls (avoid a circular include with Runtime.hpp / ThreadCache.hpp)
  struct Runtime;
  class ThreadCache;

  // CycleCollector implements synchronous cycle collection ("trial deletion")
  // as described by Bacon & Rajan, "Concurrent Cycle Collection in Reference
  // Counted Systems" (2001). It is the natural complement to reference
  // counting: plain refcounting can never reclaim a *cycle* of handles that
  // only reference each other, because every member keeps a non-zero count.
  //
  // This collector lives inside Anchorage. As Deutsch & Bobrow noted, heap
  // compaction and cycle collection are duals -- both walk the object graph
  // while the world is stopped. We therefore piggy-back on the exact same
  // stop-the-world barrier that Anchorage already uses to move objects around
  // (see `alaska::Runtime::with_barrier`). The refcount itself is the one the
  // application already maintains in the spare bits of each handle table
  // `Mapping` (see `Mapping::rc.refcount`).
  //
  // The algorithm uses two pieces of out-of-band, per-object state -- a "color"
  // and a "buffered" flag. The `Mapping` is a fully packed 8 bytes with no room
  // to spare, so we keep that state in side tables here instead of stealing
  // more bits from the handle.
  class CycleCollector {
   public:
    enum class Color : uint8_t {
      Black = 0,  // in use or free (the default for any handle we've not touched)
      Gray,       // possible member of a cycle (being traced)
      White,      // member of a garbage cycle (to be collected)
      Purple,     // possible root of a cycle (buffered as a candidate)
    };

    explicit CycleCollector(alaska::Runtime &rt)
        : rt(rt) {}

    virtual ~CycleCollector() = default;

    // PossibleRoot(S): record a handle whose refcount was just decremented to a
    // *non-zero* value. Such a handle is the only kind that can be the root of a
    // garbage cycle, so it is buffered ("purple") for the next collection.
    // Safe to call concurrently from application threads.
    void register_candidate(alaska::Mapping *m);

    // Drop any knowledge of a handle. Must be called when a handle is freed
    // through the normal path (hfree) so that we never trace a recycled slot.
    void forget(alaska::Mapping *m);

    // Run one synchronous cycle collection and return the number of objects
    // reclaimed. This walks and mutates the heap, so it MUST be called with the
    // world stopped -- i.e. from inside `Runtime::with_barrier`. `tc` is used to
    // query object sizes and to free reclaimed handles; because the barrier
    // already holds every thread cache lock on this thread, pass the *raw*
    // ThreadCache (do not wrap it in a LockedThreadCache).
    size_t collect(alaska::ThreadCache &tc);

    size_t candidate_count(void) const {
      return __atomic_load_n(&total_buffered, __ATOMIC_RELAXED);
    }
    size_t total_collected(void) const { return num_collected; }

   protected:
    // --- heap-facing operations, isolated so the trial-deletion algorithm
    // above does not depend on the in-memory layout of an object. Tests
    // override these to drive a synthetic object graph. ---

    // Append every child handle of `m` to `out` (the caller clears it first). The
    // default scans `m`'s object data word by word and treats any word that decodes
    // to a live handle as a child -- the same conservative scan the localizer uses
    // in walk_structure. Returning the children rather than invoking a callback keeps
    // the traversal allocation-free: the old ck::func visitor heap-allocated a
    // closure box for every edge walked, inside the stop-the-world pause.
    virtual void visit_children(
        alaska::ThreadCache &tc, alaska::Mapping *m, ck::vec<alaska::Mapping *> &out);

    // Is `m` a live handle that should be traced (and possibly collected)? The
    // default accepts any valid, non-free handle.
    virtual bool is_collectable(alaska::Mapping *m);

    // Reclaim `m` (it has been proven to be part of a garbage cycle).
    virtual void reclaim(alaska::ThreadCache &tc, alaska::Mapping *m);

   private:
    // --- the trial-deletion phases (see the paper) ---
    void mark_gray(alaska::ThreadCache &tc, alaska::Mapping *root);
    void scan(alaska::ThreadCache &tc, alaska::Mapping *root);
    void scan_black(alaska::ThreadCache &tc, alaska::Mapping *root);
    void collect_white(alaska::ThreadCache &tc, alaska::Mapping *root, ck::vec<alaska::Mapping *> &out);

    // Sparse color map: a handle not present is Black (the common case).
    Color color_of(alaska::Mapping *m) const;
    void set_color(alaska::Mapping *m, Color c);

    alaska::Runtime &rt;

    // --- Concurrent candidate intake (mutator hot path) ---------------------
    // register_candidate runs on the refcount store-barrier path: every decrement
    // to a non-zero count buffers a "purple" candidate root. A single global lock
    // here serialized every mutator thread on every pointer overwrite -- the
    // classic reason naive Bacon & Rajan is slow. Instead we shard the buffer by
    // handle address: a thread touching handle H only ever contends on H's shard,
    // so threads touching different handles never block each other. Shards are
    // drained into the collect()-time working set (below) while the world is
    // stopped. kCandidateShards must be a power of two (shard_for masks with it).
    static constexpr size_t kCandidateShards = 64;
    struct CandidateShard {
      ck::mutex lock;
      ck::vec<alaska::Mapping *> roots;     // buffered candidates, insertion order
      ck::set<alaska::Mapping *> buffered;  // live membership: dedup on add, drop on forget
    };
    CandidateShard candidate_shards[kCandidateShards];
    CandidateShard &shard_for(alaska::Mapping *m);
    // Sum of every shard's live `buffered` size, kept with relaxed atomics so the
    // forget() hfree fast path and candidate_count() never have to sum 64 shards.
    size_t total_buffered = 0;

    // --- collect()-time working set -----------------------------------------
    // Only touched inside collect(), which runs with the world stopped on a single
    // thread, so these need no lock. `roots`/`buffered` are drained from the shards
    // at the top of collect(); `colors` holds the transient trial-deletion coloring
    // (empty between collections).
    ck::vec<alaska::Mapping *> roots;       // drained candidate roots
    ck::set<alaska::Mapping *> buffered;    // membership test for `roots`
    ck::map<alaska::Mapping *, Color> colors;  // only ever holds non-Black entries

    // Scratch buffers reused by every collect() traversal so trial deletion
    // allocates nothing per node (vs. a fresh work-stack per root and a heap-boxed
    // visitor closure per edge before). scan() nests scan_black(), so scan_black
    // keeps its own pair to avoid clobbering the outer walk; mark_gray/scan/
    // collect_white never overlap and share the first.
    ck::vec<alaska::Mapping *> trace_stack;
    ck::vec<alaska::Mapping *> trace_children;
    ck::vec<alaska::Mapping *> sb_stack;
    ck::vec<alaska::Mapping *> sb_children;

    size_t num_collected = 0;
  };

}  // namespace alaska
