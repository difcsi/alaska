#include <gtest/gtest.h>
#include <alaska.h>
#include "alaska/Logger.hpp"
#include "gtest/gtest.h"
#include <vector>
#include <alaska/Heap.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/SizedPage.hpp>

class SizedPageTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Make it so we only get warnings
    alaska::set_log_level(LOG_WARN);
    sp = rt.heap.get_sizedpage(16, nullptr /* Dubious if passing null here is okay. */);
    hs = rt.handle_table.new_slab(nullptr);
  }
  void TearDown() override {}

  alaska::Mapping *alloc() {
    alaska::Mapping *m = hs->alloc();
    EXPECT_NE(m, nullptr);

    void *d = sp->alloc(*m, 16);
    if (d == NULL) {
      hs->release_local(m);
      return nullptr;
    }

    m->set_pointer(d);
    return m;
  }
  void release(alaska::Mapping *m) {
    sp->release_local(*m, m->get_pointer());
    hs->release_local(m);
  }

  alaska::Runtime rt;
  alaska::HandleSlab *hs;
  alaska::SizedPage *sp;
};


TEST_F(SizedPageTest, Sanity) { EXPECT_NE(sp, nullptr); }
TEST_F(SizedPageTest, AllocationWorks) {
  // Make sure we can allocate a mapping
  EXPECT_NE(alloc(), nullptr);
}

TEST_F(SizedPageTest, AllocationIsUnique) {
  // Make sure we can allocate a mapping
  alaska::Mapping *m1 = alloc();
  alaska::Mapping *m2 = alloc();

  EXPECT_NE(m1, m2);
}


TEST_F(SizedPageTest, Reuse) {
  alaska::Mapping *m1 = alloc();
  release(m1);
  alaska::Mapping *m2 = alloc();
  EXPECT_EQ(m1, m2);
}


TEST_F(SizedPageTest, CompactionNoOp) {
  // An empty heap should not be compacted.
  long amount = sp->compact();
  EXPECT_EQ(amount, 0);
}

TEST_F(SizedPageTest, CompactionFull) {
  // A heap that has no holes should not be compaacted
  auto m = alloc();
  long amount = sp->compact();
  EXPECT_EQ(amount, 0);
}

TEST_F(SizedPageTest, CompactionWithHolesAtEnd) {
  // No objects should move if the holes are only at the end of the heap.
  alaska::Mapping *m1 = alloc();  // slot 1
  alaska::Mapping *m2 = alloc();  // slot 2
  release(m2);                    // release slot 2 (end)
  long amount = sp->compact();
  EXPECT_EQ(amount, 0);
}

TEST_F(SizedPageTest, CompactionSimple) {
  // this test is a little contrived.
  // We allocate three mappings and free the middle one.
  // This leads to a heap which looks like this: [#.#....]
  // compaction should move the last mapping to the second location.
  // So after we should see this: [##.....]
  alaska::Mapping *m1 = alloc();
  alaska::Mapping *m2 = alloc();
  alaska::Mapping *m3 = alloc();

  // after compaction, m3 should be located at m2's location.
  auto m3_expected = m2->get_pointer();
  // release m2, leaving a hole behind
  release(m2);

  auto frag_start = sp->fragmentation();
  // cause the compact
  long amount = sp->compact();
  EXPECT_EQ(amount, 1);
  auto frag_end = sp->fragmentation();
  // fragmentation should have improved.
  EXPECT_LT(frag_end, frag_start);


  auto m3_actual = m3->get_pointer();
  EXPECT_EQ(m3_expected, m3_actual);
}

TEST_F(SizedPageTest, CompactionWithPinned) {
  // this test is similar to above, but we handle pinned objects.

  alaska::Mapping *dummy = alloc();
  alaska::Mapping *to_free = alloc();  // allocation to be freed
  alaska::Mapping *pinned = alloc();   // pinned allocation.
  alaska::Mapping *to_move = alloc();  // allocation to be moved to m2

  pinned->set_pinned(true);                      // pin the pinned handle so it cannot move.
  auto pinned_expected = pinned->get_pointer();  // the pinned object should not move.

  // after compaction, m4 should be located at m2's location.
  auto expected = to_free->get_pointer();
  release(to_free);

  // run compaction
  long amount = sp->compact();
  EXPECT_EQ(amount, 1);

  // check that the pinned object has not moved.
  auto pinned_actual = pinned->get_pointer();
  EXPECT_EQ(pinned_expected, pinned_actual);

  // check that the moved object has moved to the expected location.
  auto to_move_actual = to_move->get_pointer();
  EXPECT_EQ(expected, to_move_actual);
}

TEST_F(SizedPageTest, CompactionFreelistValid) {
  // This test is a little more complex.  in the event that we have a heap that
  // has a hole at the start, and a pinned object after it, we should not move
  // any objects, and the next allocation should be to the first hole.

  alaska::Mapping *to_free = alloc();  // allocation to be freed
  alaska::Mapping *pinned = alloc();   // pinned allocation.

  pinned->set_pinned(true);
  auto pinned_expected = pinned->get_pointer();  // the pinned object should not move.
  auto expected = to_free->get_pointer();        // the free object should not move.
  release(to_free);

  // run compaction
  long amount = sp->compact();
  EXPECT_EQ(amount, 0);

  // check that the pinned object has not moved.
  auto pinned_actual = pinned->get_pointer();
  EXPECT_EQ(pinned_expected, pinned_actual);


  // We now be able to allocate a new object, and it return
  // the same pointer as the freed slot


  alaska::Mapping *m = alloc();
  ASSERT_NE(m, nullptr);
  // check that the new object has the same pointer as the freed slot.
  auto actual = m->get_pointer();
  EXPECT_EQ(expected, actual);
}