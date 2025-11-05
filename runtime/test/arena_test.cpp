#include <gtest/gtest.h>
#include <alaska.h>
#include "alaska/Logger.hpp"
#include "gtest/gtest.h"
#include <vector>
#include <gmock/gmock.h>
#include <alaska/Heap.hpp>

#include <alaska/Runtime.hpp>
#include <alaska/Arena.hpp>

class ArenaTest : public ::testing::Test {
 public:
  void SetUp() override {}
  void TearDown() override {}

  alaska::Arena arena = {512 * 4096, true};
};




TEST_F(ArenaTest, Push) {
  auto ptr = arena.push<int>();
  EXPECT_NE(ptr, nullptr);
}


TEST_F(ArenaTest, PushTwo) {
  auto ptr1 = arena.push<int>();
  auto ptr2 = arena.push<int>();
  EXPECT_NE(ptr1, nullptr);
  EXPECT_NE(ptr2, nullptr);
  EXPECT_NE(ptr1, ptr2);
}


TEST_F(ArenaTest, PushMany) {
  for (int i = 0; i < 100; i++) {
    auto ptr = arena.push(512);
    EXPECT_NE(ptr, nullptr);
  }
}




// TEST_F(ArenaTest, PushDecrementsRemaining) {
//   size_t start = arena.remaining();
//   auto ptr = arena.push(64);  
//   size_t end  = arena.remaining();
//   EXPECT_EQ(start - end, 64);
// }
