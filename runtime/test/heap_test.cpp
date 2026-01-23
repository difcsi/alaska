#include <gtest/gtest.h>
#include <alaska.h>
#include "alaska/Configuration.hpp"
#include "alaska/util/Logger.hpp"
#include "alaska/heaps/SizeClass.hpp"
#include "gtest/gtest.h"
#include <vector>
#include <alaska/heaps/Heap.hpp>

#include <alaska/core/Runtime.hpp>


static alaska::Configuration g_config;
class HeapTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Make it so we only get warnings
    alaska::set_log_level(LOG_WARN);
  }
  void TearDown() override {}

  alaska::Heap heap{g_config};
};


TEST_F(HeapTest, HeapSanity) {
  // This test is just to make sure that the heap is correctly initialized.
}




TEST_F(HeapTest, SizedPageGet) {
  auto sp = heap.get_sizedpage(16);
  ASSERT_NE(sp, nullptr);
}


TEST_F(HeapTest, LocalityPageGet) {
  size_t size_req = 32;
  auto lp = heap.get_localitypage(size_req);
  ASSERT_NE(lp, nullptr);
  alaska::Mapping m;
  // ASSERT_NE(lp->alloc(m, 32), nullptr);
}



TEST_F(HeapTest, LocalityGetPutGet) {
  size_t size_req = 32;
  // Allocating a locality page then putting it back should return it
  // again to promote reuse. Asserting this might be restrictive on
  // future policy, but for now it's good enough.
  auto lp = heap.get_localitypage(size_req);
  ASSERT_NE(lp, nullptr);
  heap.put_page(lp);

  auto lp2 = heap.get_localitypage(size_req);
  ASSERT_NE(lp2, nullptr);
  ASSERT_EQ(lp, lp2);
}
