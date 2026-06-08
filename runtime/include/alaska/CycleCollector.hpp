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

    size_t candidate_count(void) const { return num_buffered; }
    size_t total_collected(void) const { return num_collected; }

   protected:
    // --- heap-facing operations, isolated so the trial-deletion algorithm
    // above does not depend on the in-memory layout of an object. Tests
    // override these to drive a synthetic object graph. ---

    // Invoke `fn` for every child handle of `m`. The default scans `m`'s object
    // data word by word and treats any word that decodes to a live handle as a
    // child -- the same conservative scan the localizer uses in walk_structure.
    virtual void visit_children(
        alaska::ThreadCache &tc, alaska::Mapping *m, const ck::func<void(alaska::Mapping *)> &fn);

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

    ck::mutex lock;
    ck::vec<alaska::Mapping *> roots;       // the candidate (purple) buffer
    ck::set<alaska::Mapping *> buffered;    // membership test for `roots`
    ck::map<alaska::Mapping *, Color> colors;  // only ever holds non-Black entries

    // `num_buffered` is read without the lock on the hfree fast path, so keep it
    // volatile. It is only an optimization hint; correctness does not depend on
    // it being perfectly in sync.
    volatile size_t num_buffered = 0;
    size_t num_collected = 0;
  };

}  // namespace alaska
