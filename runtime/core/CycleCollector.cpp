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
// threads is `register_candidate`/`forget`, which are guarded by `lock`.

#include <assert.h>  // ck/func.h uses assert() but does not include it
#include <alaska/CycleCollector.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>

// Hand-off to the stackscan reclaim path (rt/refcount.cpp). Weak because this
// translation unit links into alaska_core_static, which the unit tests build
// without refcount.cpp; there the cycle test overrides reclaim() so the default
// below is never reached and a null bridge is fine.
extern "C" void alaska_nullcount_add(void *handle) __attribute__((weak));

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
    return m != nullptr && rt.handle_table.valid_handle(m) && not m->is_free();
  }

  // A "child" is any word in the object that decodes to a live handle. This is
  // deliberately the same conservative scan the localizer uses (walk_structure):
  // a word that merely looks like a handle is treated as one. Because the
  // algorithm only ever *trial* decrements and then restores live nodes, a false
  // edge can at worst keep real garbage alive -- it can never free a live
  // object whose refcount is accurate.
  void CycleCollector::visit_children(
      alaska::ThreadCache &tc, alaska::Mapping *m, const ck::func<void(alaska::Mapping *)> &fn) {
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
      fn(child);
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

  // PossibleRoot(S)
  void CycleCollector::register_candidate(alaska::Mapping *m) {
    if (m == nullptr) return;
    ck::scoped_lock l(lock);
    if (color_of(m) == Color::Purple) return;  // already buffered
    set_color(m, Color::Purple);
    if (not buffered.contains(m)) {
      buffered.add(m);
      roots.push(m);
      num_buffered = buffered.size();
    }
  }

  void CycleCollector::forget(alaska::Mapping *m) {
    if (m == nullptr) return;
    // Fast path: nothing is buffered, so there is nothing to forget. This keeps
    // the common hfree path lock-free.
    if (num_buffered == 0) return;
    ck::scoped_lock l(lock);
    if (buffered.contains(m)) {
      buffered.remove(m);
      // We leave the (now dangling) entry in `roots`; collect() filters out any
      // root that is no longer Purple.
      num_buffered = buffered.size();
    }
    set_color(m, Color::Black);
  }

  // MarkGray(S), iterative. Each node is expanded once (the first time it is
  // popped while not already gray); every out-edge of an expanded node performs
  // exactly one trial decrement, matching the recursive form.
  void CycleCollector::mark_gray(alaska::ThreadCache &tc, alaska::Mapping *root) {
    ck::vec<alaska::Mapping *> stack;
    stack.push(root);
    while (not stack.is_empty()) {
      auto *s = pop_back(stack);
      if (color_of(s) == Color::Gray) continue;
      set_color(s, Color::Gray);
      visit_children(tc, s, [&](alaska::Mapping *t) {
        t->dec_refcount();  // subtract this internal edge
        stack.push(t);
      });
    }
  }

  // Scan(S), iterative.
  void CycleCollector::scan(alaska::ThreadCache &tc, alaska::Mapping *root) {
    ck::vec<alaska::Mapping *> stack;
    stack.push(root);
    while (not stack.is_empty()) {
      auto *s = pop_back(stack);
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
        visit_children(tc, s, [&](alaska::Mapping *t) { stack.push(t); });
      }
    }
  }

  // ScanBlack(S), iterative. Restores the refcounts that mark_gray subtracted
  // for everything reachable from a live node.
  void CycleCollector::scan_black(alaska::ThreadCache &tc, alaska::Mapping *root) {
    ck::vec<alaska::Mapping *> stack;
    stack.push(root);
    while (not stack.is_empty()) {
      auto *s = pop_back(stack);
      if (color_of(s) == Color::Black) continue;
      set_color(s, Color::Black);
      visit_children(tc, s, [&](alaska::Mapping *t) {
        t->inc_refcount();  // put back the edge mark_gray removed
        if (color_of(t) != Color::Black) stack.push(t);
      });
    }
  }

  // CollectWhite(S), iterative. Rather than free in place (which would tear down
  // an object's data before we have finished reading its children), we colour
  // the whole white component black and append its members to `out`; the caller
  // frees them afterwards.
  void CycleCollector::collect_white(
      alaska::ThreadCache &tc, alaska::Mapping *root, ck::vec<alaska::Mapping *> &out) {
    ck::vec<alaska::Mapping *> stack;
    stack.push(root);
    while (not stack.is_empty()) {
      auto *s = pop_back(stack);
      if (color_of(s) != Color::White) continue;
      if (buffered.contains(s)) continue;
      set_color(s, Color::Black);
      visit_children(tc, s, [&](alaska::Mapping *t) { stack.push(t); });
      out.push(s);
    }
  }

  size_t CycleCollector::collect(alaska::ThreadCache &tc) {
    ck::vec<alaska::Mapping *> to_free;

    {
      ck::scoped_lock l(lock);

      // MarkRoots: keep only the genuine purple candidates; everything else is
      // no longer a possible cycle root and is dropped from the buffer.
      ck::vec<alaska::Mapping *> work;
      for (auto *s : roots) {
        bool is_candidate =
            color_of(s) == Color::Purple && is_collectable(s) && s->get_refcount() > 0;
        if (is_candidate) {
          work.push(s);
        } else {
          buffered.remove(s);
          set_color(s, Color::Black);
        }
      }
      roots.clear();

      for (auto *s : work)
        mark_gray(tc, s);
      for (auto *s : work)
        scan(tc, s);
      for (auto *s : work) {
        buffered.remove(s);
        collect_white(tc, s, to_free);
      }

      num_buffered = buffered.size();
    }

    // Hand the reclaimed handles to the stackscan reclaim path. We do this outside
    // `lock` because reclaim() takes the (separate) nullcount lock; keeping the two
    // lock regions disjoint avoids any ordering coupling. We are still inside the
    // barrier, so the world remains stopped and this is safe.
    size_t freed = 0;
    for (auto *s : to_free) {
      reclaim(tc, s);
      freed++;
    }
    num_collected += freed;
    return freed;
  }

}  // namespace alaska
