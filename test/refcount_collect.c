// Integration test: a handle whose reference count falls to zero -- and that is
// not pinned on any thread's stack -- must be reclaimed automatically, "in time",
// by Anchorage's background barrier thread. No explicit hfree, no forced barrier:
// we only drop references and wait, then prove the runtime collected the garbage.
//
// HOW THE RUNTIME RECLAIMS (see runtime/rt/init.cpp:barrier_thread_func and
// runtime/rt/refcount.cpp:reclaim_dead_handles): the barrier thread wakes every
// ~50ms and, throttled to a 250ms minimum interval, takes a stop-the-world
// barrier. While the world is stopped each thread conservatively marks the
// handles found on its stack + saved registers ("present"); the reclaim pass
// then hfree's every zero-refcount handle that is in the nullcount map and is
// NOT present. So a handle leaves the nullcount map exactly when it is freed.
// We therefore use nullcount map size as the observable: it rises as we drop
// references and must drain back to baseline once the collector runs.
//
// We drive reference counts through the public refcount API (the very functions
// the compiler's store barriers call) rather than relying on compiler-inserted
// barriers, so the test deterministically exercises the *runtime's* reclamation
// path regardless of the state of the refcount compiler pass.

#include <alaska.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if !ALASKA_ENABLE_REFCOUNT

// Reclamation is a refcount-only feature. Without it there is nothing to test;
// report and succeed so the target stays green in both build configurations.
int main(void) {
  printf("[refcount_collect] ALASKA_ENABLE_REFCOUNT is off; skipping.\n");
  return 0;
}

#else

#define NUM_HANDLES 512
#define OBJ_SIZE 64

// 5s budget. The collector takes a barrier at most every 250ms, so this leaves
// ~20 reclaim opportunities -- generous headroom over the single barrier needed.
#define DEADLINE_MS 5000
#define POLL_MS 20

// Create NUM_HANDLES handles, give each a reference and then immediately drop it,
// so every handle ends at refcount 0 and lands in the nullcount map. This runs in
// its own (noinline) frame and returns nothing: the handle values live only here,
// so once it returns they are on no live stack slot or callee-saved register and
// the conservative present-scan cannot keep them alive.
__attribute__((noinline)) static void create_unreferenced_handles(void) {
  for (int i = 0; i < NUM_HANDLES; i++) {
    void *obj = halloc(OBJ_SIZE);
    if (obj == NULL) {
      fprintf(stderr, "[refcount_collect] halloc failed at %d\n", i);
      exit(1);
    }
    // Model a reference being taken (e.g. a heap store) and then released: the
    // refcount rises to 1, leaving the nullcount map, then falls back to 0,
    // re-entering it as collectable garbage.
    alaska_inc_refcount(obj);
    alaska_dec_refcount(obj);
  }
}

// Overwrite the stack region that create_unreferenced_handles used with
// non-handle words. The present-scan is conservative: a leftover copy of a
// handle value in a dead stack slot would be (correctly, safely) treated as a
// live root and pin that one handle forever. Scrubbing that region removes the
// stale words so reclamation is observable to completion rather than leaving a
// conservative residual. `volatile` keeps the writes from being optimized away.
__attribute__((noinline)) static void scrub_stack(void) {
  volatile uintptr_t buf[1024];
  for (int i = 0; i < 1024; i++) buf[i] = (uintptr_t)i;  // small, never a handle
}

static unsigned long now_ms(void) { return alaska_timestamp() / 1000000UL; }

int main(void) {
  // Baseline: handles already sitting at refcount 0 before we start (expected 0,
  // but measure rather than assume so the test is robust to runtime internals).
  int baseline = alaska_nullcount_map_size();
  printf("[refcount_collect] baseline nullcount map size = %d\n", baseline);

  create_unreferenced_handles();
  scrub_stack();  // erase any stale handle words left on the stack (see above)

  int after = alaska_nullcount_map_size();
  printf("[refcount_collect] after dropping %d references, nullcount map size = %d\n",
      NUM_HANDLES, after);

  // The handles must actually have entered the collectable set; otherwise a
  // "drain to baseline" below would pass vacuously. The collector may already
  // have reclaimed some, so only require that a clear majority showed up.
  if (after < baseline + NUM_HANDLES / 2) {
    fprintf(stderr,
        "[refcount_collect] FAIL: expected ~%d dead handles, only saw %d -- "
        "references were not dropped as expected.\n",
        NUM_HANDLES, after - baseline);
    return 1;
  }

  // Now just wait. We never call hfree and never force a barrier: the background
  // barrier thread must notice these zero-refcount, off-stack handles and reclaim
  // them on its own. A handle leaves the nullcount map only when it is freed, so
  // draining back to baseline proves they were collected.
  unsigned long start = now_ms();
  int size = after;
  while (size > baseline && (now_ms() - start) < DEADLINE_MS) {
    usleep(POLL_MS * 1000);
    size = alaska_nullcount_map_size();
  }

  unsigned long elapsed = now_ms() - start;
  if (size > baseline) {
    fprintf(stderr,
        "[refcount_collect] FAIL: %d unreferenced handle(s) still uncollected "
        "after %lums -- the background collector did not reclaim them in time.\n",
        size - baseline, elapsed);
    return 1;
  }

  printf("[refcount_collect] PASS: all %d unreferenced handles reclaimed within %lums.\n",
      NUM_HANDLES, elapsed);
  return 0;
}

#endif  // ALASKA_ENABLE_REFCOUNT
