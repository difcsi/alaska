// Integration test: multithreaded reference-count reclamation.
//
// This is the concurrent counterpart of refcount_collect.c. Several mutator
// threads each create handles, give each a reference and immediately drop it, so
// every handle reaches refcount 0 and enters the runtime's nullcount map. We then
// prove the background barrier thread reclaims all of that garbage automatically
// -- WHILE every producer thread is still alive and parked.
//
// Why keep the workers alive: Anchorage reclaims with a stop-the-world barrier
// (runtime/rt/init.cpp + runtime/rt/refcount.cpp:reclaim_dead_handles). The
// barrier signals (SIGUSR2) every registered thread and waits for all of them to
// rendezvous; each thread conservatively marks its own stack + registers present
// before parking. If the collector could not stop a thread that is blocked in a
// syscall (here, parked in pthread_barrier_wait's futex), the world-stop would
// hang and nothing would be reclaimed. So holding N producer threads at a barrier
// across the collection is exactly what exercises *multithreaded* collection: the
// reclaim has to coordinate main + all workers, not just one thread.
//
// As in refcount_collect.c we drive refcounts through the public refcount API
// (the same functions the compiler's store barriers call) so the test
// deterministically targets the runtime's reclamation path, and each thread
// scrubs its own stack after producing so the conservative present-scan does not
// pin a stale handle word and leave a residual.

#include <alaska.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if !ALASKA_ENABLE_REFCOUNT

int main(void) {
  printf("[refcount_collect_mt] ALASKA_ENABLE_REFCOUNT is off; skipping.\n");
  return 0;
}

#else

#include <pthread.h>

#define NUM_THREADS 8
#define HANDLES_PER_THREAD 256
#define TOTAL_HANDLES (NUM_THREADS * HANDLES_PER_THREAD)
#define OBJ_SIZE 64

// More threads to rendezvous per barrier (250ms min interval) than the
// single-thread test, so allow a roomier budget. One or two barriers suffice.
#define DEADLINE_MS 10000
#define POLL_MS 20

// main + every worker meet here once all garbage is produced, and again once main
// has finished verifying reclamation. Between the two rendezvous the workers are
// blocked on the second barrier (a futex), i.e. alive but off-CPU, forcing the
// collector to stop them via signal to complete its world-stop.
static pthread_barrier_t produced;
static pthread_barrier_t verified;

// Allocate HANDLES_PER_THREAD handles and drop each one's single reference, in a
// noinline frame that returns nothing: the handle values live only here, so after
// it returns they are on no live stack slot or callee-saved register.
__attribute__((noinline)) static void produce_garbage(void) {
  for (int i = 0; i < HANDLES_PER_THREAD; i++) {
    void *obj = halloc(OBJ_SIZE);
    if (obj == NULL) {
      fprintf(stderr, "[refcount_collect_mt] halloc failed\n");
      exit(1);
    }
    // Reference taken (refcount 1, leaves the nullcount map) then released
    // (refcount 0, re-enters it as collectable garbage).
    alaska_inc_refcount(obj);
    alaska_dec_refcount(obj);
  }
}

// Overwrite this thread's stack region used by produce_garbage with non-handle
// words, so the conservative present-scan cannot mistake a leftover handle copy
// for a live root and pin it (see refcount_collect.c for the full rationale).
__attribute__((noinline)) static void scrub_stack(void) {
  volatile uintptr_t buf[1024];
  for (int i = 0; i < 1024; i++) buf[i] = (uintptr_t)i;
}

static void *worker(void *arg) {
  (void)arg;
  produce_garbage();
  scrub_stack();
  // Announce "my garbage is published" and then park, staying alive (and thus a
  // mandatory participant in every collection barrier) until main has verified.
  pthread_barrier_wait(&produced);
  pthread_barrier_wait(&verified);
  return NULL;
}

static unsigned long now_ms(void) { return alaska_timestamp() / 1000000UL; }

int main(void) {
  int baseline = alaska_nullcount_map_size();
  printf("[refcount_collect_mt] %d threads x %d handles; baseline nullcount = %d\n",
      NUM_THREADS, HANDLES_PER_THREAD, baseline);

  pthread_barrier_init(&produced, NULL, NUM_THREADS + 1);
  pthread_barrier_init(&verified, NULL, NUM_THREADS + 1);

  pthread_t threads[NUM_THREADS];
  for (int i = 0; i < NUM_THREADS; i++) {
    if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
      fprintf(stderr, "[refcount_collect_mt] pthread_create failed\n");
      return 1;
    }
  }

  // Wait until all workers have produced and dropped their references. They are
  // now blocked on `verified`, alive, holding no live reference to their garbage.
  pthread_barrier_wait(&produced);

  int after = alaska_nullcount_map_size();
  printf("[refcount_collect_mt] all threads produced; nullcount = %d\n", after);

  // Sanity: the garbage must actually have entered the collectable set, or the
  // drain below would pass vacuously. The collector may already have reclaimed
  // some across threads, so require only a clear majority.
  if (after < baseline + TOTAL_HANDLES / 2) {
    fprintf(stderr,
        "[refcount_collect_mt] FAIL: expected ~%d dead handles across threads, "
        "only saw %d.\n",
        TOTAL_HANDLES, after - baseline);
    return 1;
  }

  // Wait for the background collector to drain the map back to baseline. Every
  // collection here must stop main + all %d parked workers. We never force a
  // barrier and never hfree -- reclamation is entirely the runtime's doing.
  unsigned long start = now_ms();
  int size = after;
  while (size > baseline && (now_ms() - start) < DEADLINE_MS) {
    usleep(POLL_MS * 1000);
    size = alaska_nullcount_map_size();
  }
  unsigned long elapsed = now_ms() - start;

  int rc = 0;
  if (size > baseline) {
    fprintf(stderr,
        "[refcount_collect_mt] FAIL: %d unreferenced handle(s) still uncollected "
        "after %lums with %d live threads -- collector did not reclaim in time.\n",
        size - baseline, elapsed, NUM_THREADS);
    rc = 1;
  } else {
    printf("[refcount_collect_mt] PASS: all %d handles from %d threads reclaimed "
           "within %lums.\n",
        TOTAL_HANDLES, NUM_THREADS, elapsed);
  }

  // Release the workers and shut down cleanly.
  pthread_barrier_wait(&verified);
  for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);

  pthread_barrier_destroy(&produced);
  pthread_barrier_destroy(&verified);
  return rc;
}

#endif  // ALASKA_ENABLE_REFCOUNT
