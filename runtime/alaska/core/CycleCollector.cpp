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

// Synchronous cycle collection for Anchorage.
//
// This is a faithful, iterative implementation of the "trial deletion"
// algorithm from Bacon & Rajan, "Concurrent Cycle Collection in Reference
// Counted Systems" (2001). The recursion in the paper is rewritten with
// explicit work-stacks so that collecting a deep structure cannot overflow the
// (anchorage) thread stack. The "expand each node exactly once" invariant that
// makes the recursive version correct is preserved by guarding expansion on the
// node's color.
//
// All graph mutation happens while the world is stopped (see
// `CycleCollector::collect`'s contract), so the per-object color/refcount state
// can be touched without atomics. The only thing that races with application
// threads is `register_candidate`/`forget`, which append to / drop from a
// per-handle-address shard guarded by that shard's own lock (so independent
// mutators do not serialize on a single global lock); collect() drains the shards
// with try_lock so it can never block on a mutator the barrier parked mid-append.

#include <assert.h>  // ck/func.h uses assert() but does not include it
#include <alaska/CycleCollector.hpp>
#include <alaska/core/Runtime.hpp>
#include <alaska/core/ThreadCache.hpp>

// Hand-off to the stackscan reclaim path (rt/refcount.cpp). Weak because this
// translation unit links into alaska_core_static, which the unit tests build
// without refcount.cpp; there the cycle test overrides reclaim() so the default
// below is never reached and a null bridge is fine.
extern "C" void alaska_nullcount_add(void *handle) __attribute__((weak));

// Ported from main-rc, adapted to dev (uses dev's ThreadCache::current()/get_size,
// HandleTable::is_live_handle, Runtime::with_barrier). The whole implementation is
// gated: with cycle collection OFF (dev default) only the stub C API below compiles,
// so the default runtime build is unaffected and cannot be broken by this module.
#if ALASKA_ENABLE_CYCLE_COLLECTION
namespace alaska {

  using Color = CycleCollector::Color;

  static inline alaska::Mapping *pop_back(ck::vec<alaska::Mapping *> &v) {
    auto *x = v.last();
    v.remove(v.size() - 1);
    return x;
  }

  Color CycleCollector::color_of(alaska::Mapping *m) const {
    auto it = colors.find(m);
    if (it == colors.end()) return Color::Black;
    return (*it).value;
  }

  void CycleCollector::set_color(alaska::Mapping *m, Color c) {
    // Keep the map sparse: Black is the default, so it is never stored. This
    // also means a freed-and-recycled handle naturally reads back as Black.
    if (c == Color::Black) {
      colors.remove(m);
    } else {
      colors.set(m, c);
    }
  }

  bool CycleCollector::is_collectable(alaska::Mapping *m) {
    // is_live_handle, not valid_handle() + !is_free(): a slot on the handle allocator's free
    // list keeps its invl bit clear (its word is a raw next-link), so is_free() reports it
    // live -- and trial deletion's inc/dec_refcount would then CAS-corrupt that link. See
    // HandleTable::is_live_handle.
    return rt.handle_table.is_live_handle(m);
  }

  // A "child" is any word in the object that decodes to a live handle. This is
  // deliberately the same conservative scan the localizer uses (walk_structure):
  // a word that merely looks like a handle is treated as one. Because the
  // algorithm only ever *trial* decrements and then restores live nodes, a false
  // edge can at worst keep real garbage alive -- it can never free a live
  // object whose refcount is accurate.
  void CycleCollector::visit_children(
      alaska::ThreadCache &tc, alaska::Mapping *m, ck::vec<alaska::Mapping *> &out) {
    if (m == nullptr || m->is_free()) return;
    void *handle = m->to_handle();
    size_t size = tc.get_size(handle);
    if (size < sizeof(void *)) return;

    void **words = (void **)m->get_pointer();
    if (words == nullptr) return;
    size_t n = size / sizeof(void *);
    for (size_t i = 0; i < n; i++) {
      auto *child = alaska::Mapping::from_handle_safe(words[i]);
      if (not is_collectable(child)) continue;
      out.push(child);
    }
  }

  // A handle proven to be part of a garbage cycle. We deliberately do NOT free it
  // here: the cycle collector only *breaks* cycles. Trial deletion has driven this
  // handle's refcount to 0, but a word on some thread's stack might still point at
  // it (conservatively), and the collector's pin view is not the authoritative
  // check. So we hand it to the zero-refcount set and let the in-barrier stackscan
  // reclaim (reclaim_dead_handles) do the actual free -- but only after the
  // conservative stack scan confirms the handle is off every thread's stack, which
  // is where its liballocs "heap tag" is finally dropped. Both phases run in the
  // same barrier, so a genuinely dead member is reclaimed this same cycle.
  void CycleCollector::reclaim(alaska::ThreadCache &tc, alaska::Mapping *m) {
    (void)tc;
    if (m == nullptr || m->is_free()) return;
    if (&alaska_nullcount_add) alaska_nullcount_add(m->to_handle());
  }

  // Map a handle to its candidate shard. The low bits of a Mapping pointer are
  // alignment zeros, so shift them off before masking to spread handles evenly.
  CycleCollector::CandidateShard &CycleCollector::shard_for(alaska::Mapping *m) {
    uintptr_t x = reinterpret_cast<uintptr_t>(m) >> 4;
    return candidate_shards[x & (kCandidateShards - 1)];
  }

  // PossibleRoot(S). Hot path: append to this handle's shard only. No global lock,
  // and no `colors` write -- buffer membership *is* the "purple" marker now, and the
  // transient coloring is established later, in collect(), under no contention.
  void CycleCollector::register_candidate(alaska::Mapping *m) {
    if (m == nullptr) return;
    auto &sh = shard_for(m);
    ck::scoped_lock l(sh.lock);
    if (sh.buffered.contains(m)) return;  // already buffered (dedup within the shard)
    sh.buffered.add(m);
    sh.roots.push(m);
    __atomic_fetch_add(&total_buffered, 1, __ATOMIC_RELAXED);
  }

  void CycleCollector::forget(alaska::Mapping *m) {
    if (m == nullptr) return;
    // Fast path: nothing is buffered anywhere, so there is nothing to forget. Keeps
    // the common hfree path off the shard locks entirely.
    if (__atomic_load_n(&total_buffered, __ATOMIC_RELAXED) == 0) return;
    auto &sh = shard_for(m);
    ck::scoped_lock l(sh.lock);
    if (sh.buffered.contains(m)) {
      sh.buffered.remove(m);
      // The (now stale) entry is left in sh.roots; the collect() drain skips any
      // root no longer present in sh.buffered, so it never gets traced.
      __atomic_fetch_sub(&total_buffered, 1, __ATOMIC_RELAXED);
    }
  }

  // MarkGray(S), iterative. Each node is expanded once (the first time it is
  // popped while not already gray); every out-edge of an expanded node performs
  // exactly one trial decrement, matching the recursive form.
  void CycleCollector::mark_gray(alaska::ThreadCache &tc, alaska::Mapping *root) {
    trace_stack.clear_with_capacity();
    trace_stack.push(root);
    while (not trace_stack.is_empty()) {
      auto *s = pop_back(trace_stack);
      if (color_of(s) == Color::Gray) continue;
      set_color(s, Color::Gray);
      trace_children.clear_with_capacity();
      visit_children(tc, s, trace_children);
      for (auto *t : trace_children) {
        t->dec_refcount();  // subtract this internal edge
        trace_stack.push(t);
      }
    }
  }

  // Scan(S), iterative.
  void CycleCollector::scan(alaska::ThreadCache &tc, alaska::Mapping *root) {
    trace_stack.clear_with_capacity();
    trace_stack.push(root);
    while (not trace_stack.is_empty()) {
      auto *s = pop_back(trace_stack);
      if (color_of(s) != Color::Gray) continue;
      // A node is live if it still has an external reference (refcount > 0 after
      // trial deletion) or if it is currently pinned -- i.e. reachable directly
      // from a thread stack, which the barrier marked for us. Stack references
      // are not counted in the refcount (deferred reference counting, cf.
      // Deutsch & Bobrow), so pinning is how we see them here.
      if (s->get_refcount() > 0 || s->is_pinned()) {
        scan_black(tc, s);
      } else {
        set_color(s, Color::White);
        trace_children.clear_with_capacity();
        visit_children(tc, s, trace_children);
        for (auto *t : trace_children)
          trace_stack.push(t);
      }
    }
  }

  // ScanBlack(S), iterative. Restores the refcounts that mark_gray subtracted
  // for everything reachable from a live node.
  void CycleCollector::scan_black(alaska::ThreadCache &tc, alaska::Mapping *root) {
    // Own pair of scratch buffers: scan() calls this while its own trace_stack walk
    // is in flight, so reusing trace_stack here would corrupt the outer traversal.
    sb_stack.clear_with_capacity();
    sb_stack.push(root);
    while (not sb_stack.is_empty()) {
      auto *s = pop_back(sb_stack);
      if (color_of(s) == Color::Black) continue;
      set_color(s, Color::Black);
      sb_children.clear_with_capacity();
      visit_children(tc, s, sb_children);
      for (auto *t : sb_children) {
        t->inc_refcount();  // put back the edge mark_gray removed
        if (color_of(t) != Color::Black) sb_stack.push(t);
      }
    }
  }

  // CollectWhite(S), iterative. Rather than free in place (which would tear down
  // an object's data before we have finished reading its children), we colour
  // the whole white component black and append its members to `out`; the caller
  // frees them afterwards.
  void CycleCollector::collect_white(
      alaska::ThreadCache &tc, alaska::Mapping *root, ck::vec<alaska::Mapping *> &out) {
    trace_stack.clear_with_capacity();
    trace_stack.push(root);
    while (not trace_stack.is_empty()) {
      auto *s = pop_back(trace_stack);
      if (color_of(s) != Color::White) continue;
      if (buffered.contains(s)) continue;
      set_color(s, Color::Black);
      trace_children.clear_with_capacity();
      visit_children(tc, s, trace_children);
      for (auto *t : trace_children)
        trace_stack.push(t);
      out.push(s);
    }
  }

  size_t CycleCollector::collect(alaska::ThreadCache &tc) {
    ck::vec<alaska::Mapping *> to_free;

    {
      // Drain the per-handle candidate shards into the working buffer. The world is
      // stopped, but a mutator could be parked mid-register_candidate holding a
      // shard lock, so try_lock and SKIP a contended shard (its candidates drain
      // next cycle) -- the same non-blocking discipline reclaim_dead_handles uses.
      // Blocking here would deadlock against the parked mutator.
      roots.clear_with_capacity();
      buffered.clear();
      for (auto &sh : candidate_shards) {
        if (sh.lock.try_lock() != 0) continue;
        for (auto *m : sh.roots) {
          // Skip entries dropped by forget() (no longer in sh.buffered) and any
          // cross-shard duplicate (defensive; a handle maps to a single shard).
          if (sh.buffered.contains(m) && not buffered.contains(m)) {
            buffered.add(m);
            roots.push(m);
          }
        }
        size_t live = sh.buffered.size();
        sh.roots.clear_with_capacity();
        sh.buffered.clear();
        sh.lock.unlock();
        __atomic_fetch_sub(&total_buffered, live, __ATOMIC_RELAXED);
      }

      // MarkRoots: keep only the genuine candidates; everything else is no longer a
      // possible cycle root and is dropped from the working buffer. Candidacy is now
      // "was buffered (we are iterating drained roots) and is still a live handle
      // with refcount > 0"; the old purple-color test is subsumed by buffer
      // membership, which register_candidate no longer has to stamp on the hot path.
      ck::vec<alaska::Mapping *> work;
      for (auto *s : roots) {
        if (is_collectable(s) && s->get_refcount() > 0) {
          work.push(s);
        } else {
          buffered.remove(s);
        }
      }
      roots.clear_with_capacity();

      for (auto *s : work)
        mark_gray(tc, s);
      for (auto *s : work)
        scan(tc, s);
      for (auto *s : work) {
        buffered.remove(s);
        collect_white(tc, s, to_free);
      }
    }

    // Hand the reclaimed handles to the stackscan reclaim path. reclaim() takes the
    // (separate) nullcount lock; we are still inside the barrier, so the world
    // remains stopped and this is safe.
    size_t freed = 0;
    for (auto *s : to_free) {
      reclaim(tc, s);
      freed++;
    }
    num_collected += freed;
    return freed;
  }

}  // namespace alaska
#endif  // ALASKA_ENABLE_CYCLE_COLLECTION

// --- C API (ported/adapted) -------------------------------------------------
// A single process-global collector, constructed lazily against the runtime. The
// stub versions (gate off) keep alaska.h's symbols linkable with zero behavior.
#if ALASKA_ENABLE_CYCLE_COLLECTION
namespace {
  alaska::CycleCollector &cycle_collector(void) {
    static alaska::CycleCollector cc(alaska::Runtime::get());
    return cc;
  }
}  // namespace

extern "C" {

// Buffer a decremented-but-still-nonzero handle as a possible cycle root. Called
// from the decrement barrier (rt/refcount.cpp) in a cycle-collection build.
void alaska_cycle_register_candidate(void *handle) {
  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m) cycle_collector().register_candidate(m);
}

void alaska_cycle_forget(void *handle) {
  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m) cycle_collector().forget(m);
}

size_t alaska_cycle_candidate_count(void) { return cycle_collector().candidate_count(); }
size_t alaska_cycles_collected(void) { return cycle_collector().total_collected(); }

// Run one synchronous cycle collection under a stop-the-world barrier. Reclaimed
// cycle members are handed to the nullcount set (see reclaim()); the actual free
// happens on the next alaska_refcount_reclaim().
size_t alaska_collect_cycles(void) {
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  size_t freed = 0;
  rt->with_barrier([&]() {
    // The barrier holds every thread-cache lock on this thread, so the raw tc is safe.
    if (auto *tc = alaska::ThreadCache::current()) freed = cycle_collector().collect(*tc);
  });
  return freed;
}

}  // extern "C"
#else   // !ALASKA_ENABLE_CYCLE_COLLECTION -- linkable stubs
extern "C" {
void alaska_cycle_register_candidate(void *handle) { (void)handle; }
void alaska_cycle_forget(void *handle) { (void)handle; }
size_t alaska_cycle_candidate_count(void) { return 0; }
size_t alaska_cycles_collected(void) { return 0; }
size_t alaska_collect_cycles(void) { return 0; }
}  // extern "C"
#endif  // ALASKA_ENABLE_CYCLE_COLLECTION
