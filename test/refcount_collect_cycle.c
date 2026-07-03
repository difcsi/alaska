// Integration test: a *cycle* of handles that reference only each other -- and is
// reachable from no thread's stack -- must be reclaimed by Anchorage's cycle
// collector. This is the case plain reference counting can provably never handle:
// every member keeps a non-zero count (held by the next member), so it never lands
// in the nullcount map and the ordinary reclaim path (runtime/rt/refcount.cpp:
// reclaim_dead_handles) never sees it. Only trial-deletion cycle collection
// (runtime/core/CycleCollector.cpp) can prove the whole component is garbage.
//
// HOW THE RUNTIME COLLECTS A CYCLE (see runtime/rt/init.cpp:barrier_thread_func):
// the background barrier thread periodically stops the world and, when candidate
// cycle roots have accumulated, runs CycleCollector::collect. That traces the
// object graph, finds the component whose only references are internal, and -- for
// each proven-garbage member -- hands the handle to the stackscan reclaim path
// (it does NOT free in place; the conservative stack scan in the same barrier must
// first confirm the handle is off every thread's stack). `alaska_cycles_collected`
// is the cumulative count of handles the collector has proven garbage, so it is
// our observable: it must rise by the size of the cycle we planted.
//
// As in refcount_collect.c we drive reference counts through the public refcount
// API rather than relying on compiler-inserted store barriers, so the test
// exercises the runtime's collector deterministically, independent of the refcount
// compiler pass. We never call hfree and never force a barrier: the background
// collector must notice the garbage cycle and reclaim it on its own.

#include <alaska.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if !ALASKA_ENABLE_CYCLE_COLLECTION

// Cycle collection is a separately-gated feature (a superset of reference
// counting). Without it there is nothing to test; report and succeed so the
// target stays green in builds that lack cycle collection (e.g. the refcount-only
// config, where reference counting is on but the cycle collector is not built).
int main(void) {
  printf("[refcount_collect_cycle] ALASKA_ENABLE_CYCLE_COLLECTION is off; skipping.\n");
  return 0;
}

#else

#define OBJ_SIZE 64        // room for at least one handle-sized word at offset 0
#define CYCLE_LEN 2        // a <-> b

// 10s budget. The background thread attempts cycle collection roughly once per
// second (it traces far less often than it compacts), so this leaves ~10 reclaim
// opportunities -- generous headroom over the single collection actually needed.
#define DEADLINE_MS 10000
#define POLL_MS 20

static unsigned long now_ms(void) { return alaska_timestamp() / 1000000UL; }

// Build a two-node garbage cycle (a <-> b) entirely within its own (noinline)
// frame, then return nothing: the handle values live only here, so once this
// returns they are on no live stack slot or callee-saved register and the
// conservative present-scan cannot keep them alive.
__attribute__((noinline)) static void build_garbage_cycle(void) {
  void *a = halloc(OBJ_SIZE);
  void *b = halloc(OBJ_SIZE);
  if (a == NULL || b == NULL) {
    fprintf(stderr, "[refcount_collect_cycle] halloc failed\n");
    exit(1);
  }

  // Physically link the two objects in their backing memory: a's first word holds
  // handle b, and b's first word holds handle a. We write through the *translated*
  // backing as plain integers so the compiler's store-barrier pass does not also
  // adjust refcounts here -- we account for every edge explicitly below, exactly
  // like the runtime's own HeapGarbageCycleIsCollected unit test, which keeps the
  // test deterministic no matter what the refcount compiler pass does. The bit
  // pattern is identical either way, so the collector's conservative object scan
  // (visit_children) still decodes each word back into the handle it points at.
  volatile uintptr_t *ba = (volatile uintptr_t *)alaska_translate(a);
  volatile uintptr_t *bb = (volatile uintptr_t *)alaska_translate(b);
  ba[0] = (uintptr_t)b;  // a -> b
  bb[0] = (uintptr_t)a;  // b -> a

  // Account for the two internal edges. Now a.rc == b.rc == 1, and each is kept
  // alive solely by the other: a textbook garbage cycle.
  alaska_inc_refcount(b);  // b is referenced by a
  alaska_inc_refcount(a);  // a is referenced by b

  // Model an external root that briefly pointed at each node and was then dropped.
  // A decrement to a *non-zero* count is the only event that buffers a handle as a
  // possible cycle root (Bacon & Rajan "purple"); without it the collector would
  // have no candidate to trace from. The inc/dec nets to zero, so the refcounts
  // stay at 1 -- the cycle remains pure garbage.
  alaska_inc_refcount(a); alaska_dec_refcount(a);  // buffers a as a candidate root
  alaska_inc_refcount(b); alaska_dec_refcount(b);  // buffers b as a candidate root
}

// Overwrite the stack region build_garbage_cycle used with non-handle words. The
// present-scan is conservative: a leftover copy of a handle value in a dead stack
// slot would be (correctly, safely) treated as a live root and pin that member
// forever, so the cycle could never be proven garbage. Scrubbing removes the stale
// words. `volatile` keeps the writes from being optimized away.
__attribute__((noinline)) static void scrub_stack(void) {
  volatile uintptr_t buf[1024];
  for (int i = 0; i < 1024; i++) buf[i] = (uintptr_t)i;  // small, never a handle
}

int main(void) {
  // Cumulative collector count before we plant anything, so we measure only the
  // garbage this test creates (the runtime may have collected unrelated cycles).
  unsigned long collected_before = alaska_cycles_collected();
  printf("[refcount_collect_cycle] cycles_collected baseline = %lu\n", collected_before);

  build_garbage_cycle();
  scrub_stack();  // erase any stale handle words left on the stack (see above)

  // The cycle must have registered at least one candidate root; otherwise the
  // collector has nothing to trace and a "was it collected" check below would pass
  // vacuously.
  unsigned long candidates = alaska_cycle_candidate_count();
  printf("[refcount_collect_cycle] candidate cycle roots buffered = %lu\n", candidates);
  if (candidates < 1) {
    fprintf(stderr,
        "[refcount_collect_cycle] FAIL: no cycle-root candidates were buffered -- "
        "the garbage cycle was not set up as expected.\n");
    return 1;
  }

  // Now just wait. We never hfree and never force a barrier: the background
  // collector must notice this zero-external-reference, off-stack cycle, prove it
  // garbage, and reclaim it on its own. cycles_collected rises by exactly the
  // number of handles proven garbage, so it must climb by CYCLE_LEN.
  unsigned long start = now_ms();
  unsigned long collected = 0;
  while (collected < CYCLE_LEN && (now_ms() - start) < DEADLINE_MS) {
    usleep(POLL_MS * 1000);
    collected = alaska_cycles_collected() - collected_before;
  }

  unsigned long elapsed = now_ms() - start;
  if (collected < CYCLE_LEN) {
    fprintf(stderr,
        "[refcount_collect_cycle] FAIL: only %lu of %d cycle members collected "
        "after %lums -- the cycle collector did not reclaim the garbage cycle in "
        "time.\n",
        collected, CYCLE_LEN, elapsed);
    return 1;
  }

  // The candidate buffer must have drained: a completed collection clears the
  // roots it traced. (Not load-bearing for "was it collected", but a useful
  // cross-check that the collector ran to completion rather than stalling.)
  unsigned long candidates_after = alaska_cycle_candidate_count();
  if (candidates_after != 0) {
    fprintf(stderr,
        "[refcount_collect_cycle] WARN: %lu candidate root(s) still buffered after "
        "collection.\n",
        candidates_after);
  }

  printf("[refcount_collect_cycle] PASS: %d-member garbage cycle reclaimed within "
         "%lums.\n",
      CYCLE_LEN, elapsed);
  return 0;
}

#endif  // ALASKA_ENABLE_REFCOUNT
