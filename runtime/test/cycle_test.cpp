#include <gtest/gtest.h>
#include <alaska.h>
#include "alaska/Logger.hpp"
#include "gtest/gtest.h"

#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/CycleCollector.hpp>

#include <map>
#include <set>
#include <vector>
#include <string.h>

// Unit tests for Anchorage's cycle collector (Bacon & Rajan trial deletion).
//
// These exercise the *algorithm* directly on a synthetic object graph, so they
// do not depend on how a handle's object is laid out in the heap. The collector
// reaches the heap only through three virtual hooks (visit_children,
// is_collectable, reclaim); we override them here to describe a graph by hand.
// The reference counts that drive the algorithm are kept in the real `Mapping`
// nodes, exactly as in production.

namespace {

  class TestCollector : public alaska::CycleCollector {
   public:
    using alaska::CycleCollector::CycleCollector;

    // edges[m] = the handles m references. An edge from -> to contributes 1 to
    // to's reference count (see CycleTest::link).
    std::map<alaska::Mapping *, std::vector<alaska::Mapping *>> edges;
    std::set<alaska::Mapping *> freed;

   protected:
    void visit_children(alaska::ThreadCache &, alaska::Mapping *m,
        const ck::func<void(alaska::Mapping *)> &fn) override {
      auto it = edges.find(m);
      if (it == edges.end()) return;
      for (auto *c : it->second) {
        if (freed.find(c) == freed.end()) fn(c);
      }
    }

    bool is_collectable(alaska::Mapping *m) override {
      return m != nullptr && freed.find(m) == freed.end();
    }

    void reclaim(alaska::ThreadCache &, alaska::Mapping *m) override { freed.insert(m); }
  };

}  // namespace


class CycleTest : public ::testing::Test {
 public:
  void SetUp() override {
    alaska::set_log_level(LOG_WARN);
    tc = runtime.new_threadcache();
    coll = new TestCollector(runtime);
  }
  void TearDown() override {
    delete coll;
    runtime.del_threadcache(tc);
  }

  // A fresh handle: refcount 0, no color, no flags (just like halloc resets it).
  alaska::Mapping *node() {
    EXPECT_LT(used, (int)(sizeof(storage) / sizeof(storage[0])));
    auto *m = &storage[used++];
    memset(m, 0, sizeof(*m));
    return m;
  }

  // An internal edge from -> to: record it and bump to's refcount, exactly as
  // the compiler's store barrier (alaska_inc_refcount) would on a heap store.
  void link(alaska::Mapping *from, alaska::Mapping *to) {
    coll->edges[from].push_back(to);
    to->inc_refcount();
  }

  bool is_freed(alaska::Mapping *m) { return coll->freed.find(m) != coll->freed.end(); }

  alaska::Mapping storage[64];
  int used = 0;
  TestCollector *coll;
  alaska::ThreadCache *tc;
  alaska::Runtime runtime;
};


// A pure garbage cycle (two handles that only reference each other, nothing
// else) must be reclaimed.
TEST_F(CycleTest, GarbageCycleIsCollected) {
  auto *a = node();
  auto *b = node();
  link(a, b);
  link(b, a);
  // Each ends with refcount 1: exactly its single internal in-edge.
  ASSERT_EQ(a->get_refcount(), 1u);
  ASSERT_EQ(b->get_refcount(), 1u);

  coll->register_candidate(a);
  coll->register_candidate(b);
  ASSERT_EQ(coll->candidate_count(), 2u);

  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 2u);
  ASSERT_TRUE(is_freed(a));
  ASSERT_TRUE(is_freed(b));
  ASSERT_EQ(coll->candidate_count(), 0u);
  ASSERT_EQ(coll->total_collected(), 2u);
}


// The same cycle, but reachable from an outside reference, must NOT be
// collected, and trial deletion must restore refcounts exactly.
TEST_F(CycleTest, LiveCycleIsPreserved) {
  auto *a = node();
  auto *b = node();
  link(a, b);
  link(b, a);
  a->inc_refcount();  // an external reference keeping the cycle alive
  ASSERT_EQ(a->get_refcount(), 2u);
  ASSERT_EQ(b->get_refcount(), 1u);

  coll->register_candidate(a);
  coll->register_candidate(b);

  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 0u);
  ASSERT_FALSE(is_freed(a));
  ASSERT_FALSE(is_freed(b));
  ASSERT_EQ(a->get_refcount(), 2u);
  ASSERT_EQ(b->get_refcount(), 1u);
}


// A pinned handle is a live stack root: a cycle reachable from it must survive
// even though its refcount alone would look like garbage. Stack references are
// not counted in the refcount (deferred RC), so the barrier's pin flag is how
// the collector sees them.
TEST_F(CycleTest, PinnedCycleIsPreserved) {
  auto *a = node();
  auto *b = node();
  link(a, b);
  link(b, a);
  a->set_pinned(true);

  coll->register_candidate(a);
  coll->register_candidate(b);

  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 0u);
  ASSERT_FALSE(is_freed(a));
  ASSERT_FALSE(is_freed(b));
  ASSERT_EQ(a->get_refcount(), 1u);
  ASSERT_EQ(b->get_refcount(), 1u);

  a->set_pinned(false);
}


// A self-referential handle (a -> a) is the smallest possible garbage cycle.
TEST_F(CycleTest, SelfLoopIsCollected) {
  auto *a = node();
  link(a, a);
  ASSERT_EQ(a->get_refcount(), 1u);

  coll->register_candidate(a);
  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 1u);
  ASSERT_TRUE(is_freed(a));
}


// A longer garbage cycle (a -> b -> c -> a) that also points at a live object d
// (d -> c edge plus an external reference). The cycle is reclaimed; d survives,
// and its refcount drops to just its external reference once the cycle's edge
// to it is gone.
TEST_F(CycleTest, LongCycleWithLiveTail) {
  auto *a = node();
  auto *b = node();
  auto *c = node();
  auto *d = node();

  link(a, b);
  link(b, c);
  link(c, a);
  link(c, d);       // edge into the live tail
  d->inc_refcount();  // d: 1 (from c) + 1 (external) = 2

  coll->register_candidate(a);
  coll->register_candidate(b);
  coll->register_candidate(c);

  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 3u);
  ASSERT_TRUE(is_freed(a));
  ASSERT_TRUE(is_freed(b));
  ASSERT_TRUE(is_freed(c));
  ASSERT_FALSE(is_freed(d));
  // The c->d edge is gone with the cycle, so d keeps only its external ref.
  ASSERT_EQ(d->get_refcount(), 1u);
}


// Two independent garbage cycles registered together are both collected.
TEST_F(CycleTest, TwoDisjointCycles) {
  auto *a = node();
  auto *b = node();
  auto *c = node();
  auto *d = node();
  link(a, b);
  link(b, a);
  link(c, d);
  link(d, c);

  coll->register_candidate(a);
  coll->register_candidate(b);
  coll->register_candidate(c);
  coll->register_candidate(d);

  size_t reclaimed = coll->collect(*tc);

  ASSERT_EQ(reclaimed, 4u);
  ASSERT_TRUE(is_freed(a));
  ASSERT_TRUE(is_freed(b));
  ASSERT_TRUE(is_freed(c));
  ASSERT_TRUE(is_freed(d));
}


// End-to-end through the real heap: allocate two real handles that reference
// each other, then let the *real* collector (runtime.cycle_collector) scan the
// objects, prove the cycle is garbage, and hfree it. This exercises the
// conservative object scan and the real free path, not the synthetic seam.
TEST_F(CycleTest, HeapGarbageCycleIsCollected) {
  void *ha = tc->halloc(64);
  void *hb = tc->halloc(64);
  ASSERT_NE(ha, nullptr);
  ASSERT_NE(hb, nullptr);
  auto *a = alaska::Mapping::from_handle_safe(ha);
  auto *b = alaska::Mapping::from_handle_safe(hb);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_FALSE(a->is_free());
  ASSERT_FALSE(b->is_free());

  // Zero the objects, then store each handle into the other (a <-> b) and bump
  // refcounts as the compiler's store barrier would.
  memset(a->get_pointer(), 0, tc->get_size(ha));
  memset(b->get_pointer(), 0, tc->get_size(hb));
  ((void **)a->get_pointer())[0] = hb;
  b->inc_refcount();
  ((void **)b->get_pointer())[0] = ha;
  a->inc_refcount();
  ASSERT_EQ(a->get_refcount(), 1u);
  ASSERT_EQ(b->get_refcount(), 1u);

  runtime.cycle_collector.register_candidate(a);
  runtime.cycle_collector.register_candidate(b);

  // Single-threaded test: the "world" is already stopped, so we can collect
  // directly without a barrier manager.
  size_t reclaimed = runtime.cycle_collector.collect(*tc);

  // Both members of the garbage cycle are hfree'd. (We assert on the collector's
  // result rather than is_free(): once a handle is freed its slot is reused as a
  // free-list link, so is_free() no longer reflects it.)
  ASSERT_EQ(reclaimed, 2u);
  ASSERT_EQ(runtime.cycle_collector.total_collected(), 2u);
  ASSERT_EQ(runtime.cycle_collector.candidate_count(), 0u);
}


// A handle that is forgotten (freed through the normal path) before collection
// must not be treated as a candidate.
TEST_F(CycleTest, ForgottenHandleIsDropped) {
  auto *a = node();
  auto *b = node();
  link(a, b);
  link(b, a);

  coll->register_candidate(a);
  coll->register_candidate(b);
  ASSERT_EQ(coll->candidate_count(), 2u);

  // b is freed elsewhere; the collector is told to forget it.
  coll->freed.insert(b);
  coll->forget(b);
  ASSERT_EQ(coll->candidate_count(), 1u);

  size_t reclaimed = coll->collect(*tc);
  // With b gone, a is no longer part of a cycle, so nothing new is collected.
  ASSERT_EQ(reclaimed, 0u);
  ASSERT_FALSE(is_freed(a));
}
