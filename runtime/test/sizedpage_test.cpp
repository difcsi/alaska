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
