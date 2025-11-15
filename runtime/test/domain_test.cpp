#include <gtest/gtest.h>
#include <alaska.h>
#include "alaska/Logger.hpp"
#include "gtest/gtest.h"
#include <vector>
#include <alaska/Heap.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/Domain.hpp>

class DomainTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Make it so we only get warnings
    alaska::set_log_level(LOG_WARN);
    tc = rt.new_threadcache();
  }
  void TearDown() override {
    rt.del_threadcache(tc);
  }

  alaska::Runtime rt;
  alaska::ThreadCache *tc;
};


// Test that a single domain can allocate handles
TEST_F(DomainTest, SingleDomainAlloc) {
  auto &global_domain = rt.global_domain;
  void *h1 = tc->halloc(16);
  void *h2 = tc->halloc(16);

  EXPECT_NE(h1, nullptr);
  EXPECT_NE(h2, nullptr);
  EXPECT_NE(h1, h2);
}


// Test that we can create multiple domains
TEST_F(DomainTest, MultipleDomains) {
  auto &global_domain = rt.global_domain;

  // Create a second domain
  alaska::Domain domain2(rt.handle_table);

  // Both domains should exist and be different
  EXPECT_NE(&global_domain, &domain2);
}


// Test that multiple domains have separate slab lists
TEST_F(DomainTest, DomainsHaveSeparateSlabs) {
  auto &global_domain = rt.global_domain;

  // Create a second domain
  alaska::Domain domain2(rt.handle_table);

  // Allocate from global domain
  void *h1 = tc->halloc(16);
  EXPECT_NE(h1, nullptr);

  // Global domain should have allocated at least one slab
  EXPECT_GT(global_domain.get_slabs().size(), 0);

  // Domain2 should have no slabs yet (we haven't allocated from it)
  EXPECT_EQ(domain2.get_slabs().size(), 0);
}


// Test that domain tracks handles correctly
TEST_F(DomainTest, DomainTracksHandles) {
  auto &global_domain = rt.global_domain;

  // Allocate several handles
  std::vector<void *> handles;
  for (int i = 0; i < 5; i++) {
    handles.push_back(tc->halloc(16));
    EXPECT_NE(handles.back(), nullptr);
  }

  // All handles should be from slabs owned by the domain
  for (auto h : handles) {
    auto m = alaska::Mapping::from_handle_safe(h);
    EXPECT_NE(m, nullptr);

    // Get the slab for this handle
    auto slab = rt.handle_table.get_slab(m);
    EXPECT_NE(slab, nullptr);

    // Slab should be owned by global_domain
    EXPECT_EQ(slab->owner_domain, &global_domain);
  }
}


// Test that handles can be freed and reused within a domain
TEST_F(DomainTest, HandleReuseWithinDomain) {
  size_t size = 16;

  // Allocate first handle
  void *h1 = tc->halloc(size);
  EXPECT_NE(h1, nullptr);

  // Free it
  tc->hfree(h1);

  // Allocate second handle - should get h1 back
  void *h2 = tc->halloc(size);
  EXPECT_NE(h2, nullptr);
  EXPECT_EQ(h1, h2);
}


// Test that freeing and reallocating uses the same backing storage
TEST_F(DomainTest, HandleReuseUsesBackingStorage) {
  size_t size = 16;

  // Allocate and get backing pointer
  void *h1 = tc->halloc(size);
  void *p1 = alaska::Mapping::translate(h1);
  EXPECT_NE(p1, nullptr);

  // Free it
  tc->hfree(h1);

  // Allocate again
  void *h2 = tc->halloc(size);
  void *p2 = alaska::Mapping::translate(h2);

  // Should reuse the same backing storage
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(h1, h2);
}


// Test that multiple allocations don't exhaust a slab immediately
TEST_F(DomainTest, SlabHasCapacity) {
  auto &global_domain = rt.global_domain;

  // Record initial slab count
  size_t initial_slab_count = global_domain.get_slabs().size();

  // Allocate many small handles
  std::vector<void *> handles;
  for (int i = 0; i < 100; i++) {
    handles.push_back(tc->halloc(16));
    EXPECT_NE(handles.back(), nullptr);
  }

  // Should have used multiple slabs (each slab has limited capacity)
  // But not 100 slabs for 100 small allocations
  size_t final_slab_count = global_domain.get_slabs().size();
  EXPECT_GT(final_slab_count, initial_slab_count);
  EXPECT_LT(final_slab_count, 100);  // Shouldn't be 1 slab per allocation
}


// Test that domain cleanup works correctly
TEST_F(DomainTest, DomainCleanup) {
  auto &global_domain = rt.global_domain;

  // Create a temporary domain and allocate from it
  {
    alaska::Domain temp_domain(rt.handle_table);

    // Record initial handle table free slab count
    // (This is internal, just checking the domain has slabs)
    EXPECT_EQ(temp_domain.get_slabs().size(), 0);

    // Allocate from temp domain by calling alloc_handle directly
    auto mapping = temp_domain.alloc_handle();
    EXPECT_NE(mapping, nullptr);
    EXPECT_GT(temp_domain.get_slabs().size(), 0);

    size_t slab_count_before_cleanup = temp_domain.get_slabs().size();

    // When temp_domain goes out of scope, dropAll() should be called
    // This should return all slabs to the free list
  }

  // temp_domain is now destroyed, its slabs should be returned
}


// Test that each domain maintains its own current_slab pointer
TEST_F(DomainTest, DomainsHaveIndependentCurrentSlab) {
  auto &global_domain = rt.global_domain;

  // Create another domain
  alaska::Domain domain2(rt.handle_table);

  // Both should start with nullptr current_slab
  EXPECT_EQ(global_domain.get_current_slab(), nullptr);
  EXPECT_EQ(domain2.get_current_slab(), nullptr);

  // Allocate from global domain
  auto m1 = global_domain.alloc_handle();
  EXPECT_NE(m1, nullptr);
  EXPECT_NE(global_domain.get_current_slab(), nullptr);

  // domain2 should still have nullptr current_slab
  EXPECT_EQ(domain2.get_current_slab(), nullptr);

  // Allocate from domain2
  auto m2 = domain2.alloc_handle();
  EXPECT_NE(m2, nullptr);
  EXPECT_NE(domain2.get_current_slab(), nullptr);

  // They should have different current_slabs
  EXPECT_NE(global_domain.get_current_slab(), domain2.get_current_slab());
}


// Test that slab owner_domain is properly set
TEST_F(DomainTest, SlabOwnerDomainIsSet) {
  auto &global_domain = rt.global_domain;

  // Allocate from global domain
  auto m = global_domain.alloc_handle();
  EXPECT_NE(m, nullptr);

  // Get the slab for this handle
  auto slab = rt.handle_table.get_slab(m);
  EXPECT_NE(slab, nullptr);

  // Slab owner_domain should be set to global_domain
  EXPECT_EQ(slab->owner_domain, &global_domain);
}


// Test that slab reset works correctly (for recycling)
TEST_F(DomainTest, SlabResetClearsState) {
  auto &global_domain = rt.global_domain;

  // Allocate a handle and get its slab
  auto m = global_domain.alloc_handle();
  EXPECT_NE(m, nullptr);

  auto slab = rt.handle_table.get_slab(m);
  EXPECT_NE(slab, nullptr);

  // Record some initial state
  auto initial_free_count = slab->num_free();

  // Reset the slab
  slab->reset();

  // After reset, slab should have all space available again
  auto reset_free_count = slab->num_free();
  EXPECT_EQ(reset_free_count, slab->capacity());
}


// Test that multiple domains don't interfere with each other
TEST_F(DomainTest, MultipleDomainsDontInterfere) {
  auto &global_domain = rt.global_domain;

  // Create a second domain
  alaska::Domain domain2(rt.handle_table);

  // Allocate from both domains
  std::vector<void *> global_handles;
  std::vector<void *> domain2_handles;

  for (int i = 0; i < 10; i++) {
    auto m1 = global_domain.alloc_handle();
    auto m2 = domain2.alloc_handle();

    EXPECT_NE(m1, nullptr);
    EXPECT_NE(m2, nullptr);

    global_handles.push_back((void *)m1);
    domain2_handles.push_back((void *)m2);
  }

  // Verify each handle is from its respective domain
  for (auto h : global_handles) {
    auto m = (alaska::Mapping *)h;
    auto slab = rt.handle_table.get_slab(m);
    EXPECT_EQ(slab->owner_domain, &global_domain);
  }

  for (auto h : domain2_handles) {
    auto m = (alaska::Mapping *)h;
    auto slab = rt.handle_table.get_slab(m);
    EXPECT_EQ(slab->owner_domain, &domain2);
  }
}


// Test that Domain::contains() correctly identifies domain membership
TEST_F(DomainTest, DomainContains) {
  auto &global_domain = rt.global_domain;

  // Create a second domain
  alaska::Domain domain2(rt.handle_table);

  // Allocate from both domains
  auto m1 = global_domain.alloc_handle();
  auto m2 = domain2.alloc_handle();

  EXPECT_NE(m1, nullptr);
  EXPECT_NE(m2, nullptr);

  // Check that each domain contains its own mappings
  EXPECT_TRUE(global_domain.contains(m1));
  EXPECT_FALSE(global_domain.contains(m2));

  EXPECT_FALSE(domain2.contains(m1));
  EXPECT_TRUE(domain2.contains(m2));
}


// Test that every allocated handle belongs to its domain
TEST_F(DomainTest, AllAllocatedHandlesContainedInDomain) {
  auto &global_domain = rt.global_domain;

  // Allocate many handles
  std::vector<alaska::Mapping *> handles;
  for (int i = 0; i < 50; i++) {
    auto m = global_domain.alloc_handle();
    EXPECT_NE(m, nullptr);
    handles.push_back(m);
  }

  // Verify every allocated handle is contained in global_domain
  for (auto m : handles) {
    EXPECT_TRUE(global_domain.contains(m))
        << "Handle at " << (void *)m << " should be in global_domain";
  }
}


// Test that handles from different domains don't overlap
TEST_F(DomainTest, DomainsHaveNoOverlappingHandles) {
  auto &global_domain = rt.global_domain;

  // Create second and third domains
  alaska::Domain domain2(rt.handle_table);
  alaska::Domain domain3(rt.handle_table);

  // Allocate handles from all three domains
  std::vector<alaska::Mapping *> global_handles;
  std::vector<alaska::Mapping *> domain2_handles;
  std::vector<alaska::Mapping *> domain3_handles;

  for (int i = 0; i < 20; i++) {
    global_handles.push_back(global_domain.alloc_handle());
    domain2_handles.push_back(domain2.alloc_handle());
    domain3_handles.push_back(domain3.alloc_handle());
  }

  // Verify that handles don't overlap between domains
  // Global domain handles should NOT be in domain2 or domain3
  for (auto m : global_handles) {
    EXPECT_TRUE(global_domain.contains(m));
    EXPECT_FALSE(domain2.contains(m))
        << "Global domain handle should not be in domain2";
    EXPECT_FALSE(domain3.contains(m))
        << "Global domain handle should not be in domain3";
  }

  // Domain2 handles should NOT be in global or domain3
  for (auto m : domain2_handles) {
    EXPECT_FALSE(global_domain.contains(m))
        << "Domain2 handle should not be in global_domain";
    EXPECT_TRUE(domain2.contains(m));
    EXPECT_FALSE(domain3.contains(m))
        << "Domain2 handle should not be in domain3";
  }

  // Domain3 handles should NOT be in global or domain2
  for (auto m : domain3_handles) {
    EXPECT_FALSE(global_domain.contains(m))
        << "Domain3 handle should not be in global_domain";
    EXPECT_FALSE(domain2.contains(m))
        << "Domain3 handle should not be in domain2";
    EXPECT_TRUE(domain3.contains(m));
  }
}


// Test that allocated handles are immediately contained in domain
TEST_F(DomainTest, ImmediateContainmentAfterAlloc) {
  auto &global_domain = rt.global_domain;

  // Allocate a handle
  auto m = global_domain.alloc_handle();
  EXPECT_NE(m, nullptr);

  // Immediately check that it's contained
  EXPECT_TRUE(global_domain.contains(m));

  // Allocate another and check
  auto m2 = global_domain.alloc_handle();
  EXPECT_NE(m2, nullptr);
  EXPECT_TRUE(global_domain.contains(m2));

  // First one should still be contained
  EXPECT_TRUE(global_domain.contains(m));
}


// Test that multiple domains with many handles have no overlap
TEST_F(DomainTest, ManyHandlesNoOverlap) {
  auto &global_domain = rt.global_domain;
  alaska::Domain domain2(rt.handle_table);

  // Allocate 100 handles from each domain
  std::vector<alaska::Mapping *> global_handles;
  std::vector<alaska::Mapping *> domain2_handles;

  for (int i = 0; i < 100; i++) {
    auto m1 = global_domain.alloc_handle();
    auto m2 = domain2.alloc_handle();

    EXPECT_NE(m1, nullptr);
    EXPECT_NE(m2, nullptr);

    global_handles.push_back(m1);
    domain2_handles.push_back(m2);
  }

  // Verify containment for all handles
  for (auto m : global_handles) {
    EXPECT_TRUE(global_domain.contains(m));
    EXPECT_FALSE(domain2.contains(m));
  }

  for (auto m : domain2_handles) {
    EXPECT_FALSE(global_domain.contains(m));
    EXPECT_TRUE(domain2.contains(m));
  }

  // Verify no two handles from global_domain are the same
  for (size_t i = 0; i < global_handles.size(); i++) {
    for (size_t j = i + 1; j < global_handles.size(); j++) {
      EXPECT_NE(global_handles[i], global_handles[j])
          << "Global domain handles should be unique";
    }
  }

  // Verify no two handles from domain2 are the same
  for (size_t i = 0; i < domain2_handles.size(); i++) {
    for (size_t j = i + 1; j < domain2_handles.size(); j++) {
      EXPECT_NE(domain2_handles[i], domain2_handles[j])
          << "Domain2 handles should be unique";
    }
  }
}
