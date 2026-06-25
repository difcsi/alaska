// Tests for the Perceus-flavored reuse cache (always on under ALASKA_ENABLE_REFCOUNT).
//
// These exercise the SYNCHRONOUS free path directly: ThreadCache::hfree calls
// hfree_impl(handle, /*quarantine_slot=*/false), which is exactly where the cache
// fires -- so the cache is active in this fixture regardless of the deferred-free
// runtime env vars. The whole file is compiled only when refcount is on (see
// runtime/CMakeLists.txt), so we can assert cache-specific behavior outright.
//
// Key observable: the normal freelist + handle-slab path ALREADY returns the same
// handle + backing on free-then-same-size-alloc (both are LIFO), so identity reuse
// alone does not prove the cache ran. What is unique to the cache is that a stashed
// slot keeps its backing pointer set (get_pointer() == p) during the stash window,
// whereas ANY normal free first clears it (set_pointer(nullptr)) and then overwrites
// the mapping word with a handle-slab freelist link -- so get_pointer() != p. That is
// the precise invariant the optimization maintains (NOT is_free(): the slab freelist
// stores its link in the word without setting the invl bit, so is_free() stays false
// for a normally freed slot too).

#include <gtest/gtest.h>
#include <string.h>
#include <alaska/ThreadCache.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/alaska.hpp>

class ReuseCacheTest : public ::testing::Test {
 public:
  void SetUp() override {
    t1 = rt.new_threadcache();
    t2 = rt.new_threadcache();
  }
  void TearDown() override {
    rt.del_threadcache(t1);
    rt.del_threadcache(t2);
  }

  alaska::Runtime rt;
  alaska::ThreadCache *t1, *t2;
};


// A uniquely-owned (refcount 0) handle freed synchronously is STASHED, not freed:
// its slot stays live and the next same-size alloc re-hands the same handle+backing.
TEST_F(ReuseCacheTest, StashKeepsSlotLiveAndReuses) {
  void *h1 = t1->halloc(48);
  auto *m1 = alaska::Mapping::from_handle_safe(h1);
  void *p1 = alaska::Mapping::translate(h1);

  t1->hfree(h1);
  // Stashed, not returned to the handle table -> the slot still points at its backing.
  ASSERT_EQ(m1->get_pointer(), p1);

  void *h2 = t1->halloc(48);
  ASSERT_EQ(h1, h2);
  ASSERT_EQ(p1, alaska::Mapping::translate(h2));
}


// The reused backing carries stale data; halloc must zero it.
TEST_F(ReuseCacheTest, ReusedBackingIsZeroed) {
  // Request zeroing explicitly: tc->halloc defaults zero=false (only the public
  // halloc() wrapper passes zero=1). The reuse path must honor it like the normal path.
  void *h1 = t1->halloc(64, /*zero=*/true);
  void *p1 = alaska::Mapping::translate(h1);
  memset(p1, 0xAB, 64);

  t1->hfree(h1);

  void *h2 = t1->halloc(64, /*zero=*/true);
  void *p2 = alaska::Mapping::translate(h2);
  ASSERT_EQ(p1, p2);  // confirm the cache path (same backing)
  for (int i = 0; i < 64; i++)
    ASSERT_EQ(((unsigned char *)p2)[i], 0u) << "byte " << i << " not zeroed";
}


// Reusing a backing slot for a smaller request in the same class must update the
// recorded size (size_slack), so get_size() reflects the NEW request.
TEST_F(ReuseCacheTest, ReuseUpdatesSizeSlackWithinClass) {
  // 160 and 144 are distinct 16-byte-aligned sizes that both land in size class 8
  // (object_size 160), so they share a backing slot but have different size_of().
  void *h1 = t1->halloc(160);
  void *p1 = alaska::Mapping::translate(h1);
  ASSERT_EQ(t1->get_size(h1), 160u);
  t1->hfree(h1);

  void *h2 = t1->halloc(144);
  ASSERT_EQ(p1, alaska::Mapping::translate(h2));  // reused same backing
  ASSERT_EQ(t1->get_size(h2), 144u);              // slack rewritten for the new request
}


// refcount > 1 is NOT uniquely owned: the cache must decline and free normally
// (the slot ends up freelist-linked, i.e. is_free()).
TEST_F(ReuseCacheTest, RefcountGreaterThanOneDeclines) {
  void *h1 = t1->halloc(48);
  auto *m1 = alaska::Mapping::from_handle_safe(h1);
  void *p1 = alaska::Mapping::translate(h1);
  m1->inc_refcount();
  m1->inc_refcount();
  ASSERT_EQ(m1->get_refcount(), 2u);

  t1->hfree(h1);
  // Declined -> normal free path cleared/overwrote the backing pointer (not stashed).
  ASSERT_NE(m1->get_pointer(), p1);
}


// A remote free (object owned by t1, freed via t2) must NOT stash into t2's cache;
// it takes the unchanged remote-free path. t1's later same-size alloc therefore does
// not reuse the remotely-freed backing.
TEST_F(ReuseCacheTest, RemoteFreeDoesNotStash) {
  void *h1 = t1->halloc(48);
  void *p1 = alaska::Mapping::translate(h1);

  t2->hfree(h1);  // remote free (t2 does not own t1's page)

  void *h2 = t1->halloc(48);
  ASSERT_NE(p1, alaska::Mapping::translate(h2));
}


// With the per-class slot already occupied, a second same-class free cannot stash and
// must fall through to the normal free; the first (stashed) entry is reused next.
TEST_F(ReuseCacheTest, OccupiedSlotFallsThrough) {
  void *h1 = t1->halloc(48);
  void *h2 = t1->halloc(48);
  auto *m1 = alaska::Mapping::from_handle_safe(h1);
  auto *m2 = alaska::Mapping::from_handle_safe(h2);
  void *p1 = alaska::Mapping::translate(h1);
  void *p2 = alaska::Mapping::translate(h2);

  t1->hfree(h1);  // cache empty -> stashed (backing pointer preserved)
  ASSERT_EQ(m1->get_pointer(), p1);
  t1->hfree(h2);  // cache occupied -> normal free (backing pointer not preserved)
  ASSERT_NE(m2->get_pointer(), p2);

  void *h3 = t1->halloc(48);  // reuses the stashed h1
  ASSERT_EQ(h3, h1);
  ASSERT_EQ(p1, alaska::Mapping::translate(h3));
}
