// Regression test for a handle-reuse fault in the reference-count reclamation path
// (fixed in ThreadCache::hfree + reclaim_dead_handles; see runtime/rt/refcount.cpp
// alaska_nullcount_forget and the nullcount_map bit/map lockstep).
//
// THE BUG: when a heap reference is overwritten, the dec barrier drops the pointee's
// refcount; if it hits zero the handle is recorded in the GC's `nullcount_map` (the
// collectable-garbage set) and flagged with Mapping::kOnNullcountBit. An EXPLICIT
// hfree of such a handle used to free its backing store and return its mapping slot to
// the slab WITHOUT removing it from nullcount_map. The next allocation that recycled
// that slot got the same handle value -- still listed as collectable -- and the
// background stackscan reclaim, catching the reused handle at a moment it sat at
// refcount 0, freed it out from under the mutator. Reading it back then returned
// recycled memory (corruption), or crashed.
//
// THE SHAPE THAT TRIGGERS IT: build a linked list whose `next` links are real handles
// (so interior nodes reach refcount 1), then tear it down by nulling each link (dec ->
// the child hits refcount 0 and enters nullcount_map) and immediately hfree-ing the
// node (which, pre-fix, left a stale nullcount entry); rebuild a fresh list so those
// slots get recycled; repeat. With cycle collection on (refcount-gc*) the background
// collector runs between rounds and, pre-fix, reclaimed a live recycled node -- which
// surfaced deterministically as a wrong id when walking the next round's list.
//
// Self-checks and exits non-zero on any corruption / premature free / crash. On a
// correct runtime it passes on every configuration; with reclamation off (plain
// refcount) it simply can't corrupt, and serves as a smoke test there.

#include <alaska.h>
#include <stdio.h>
#include <stdlib.h>

#if !ALASKA_ENABLE_REFCOUNT

int main(void) {
  printf("[refcount_reclaim_reuse] ALASKA_ENABLE_REFCOUNT is off; skipping.\n");
  return 0;
}

#else

#define N 20000    // nodes per list
#define ROUNDS 8   // rebuild rounds; the pre-fix failure appeared at round 2

struct node {
  struct node *next;  // a real handle -> the dec/free path puts it on nullcount_map
  long id;
};

// Build a list of `n` nodes; each `p->next = head` is a real-handle store.
__attribute__((noinline)) static struct node *build(long n) {
  struct node *head = NULL;
  for (long i = 0; i < n; i++) {
    struct node *p = (struct node *)halloc(sizeof *p);
    if (!p) {
      fprintf(stderr, "[refcount_reclaim_reuse] halloc failed at %ld\n", i);
      exit(1);
    }
    p->next = head;
    p->id = i;
    head = p;
  }
  return head;
}

// Walk and check length + ids. A reused-while-live node reads back a wrong id here.
__attribute__((noinline)) static int verify(struct node *head, long n) {
  long c = 0;
  for (struct node *p = head; p; p = p->next) {
    if (p->id != n - 1 - c) return 1;
    c++;
  }
  return c == n ? 0 : 6;
}

// Tear down head-first: null each link (fires the dec barrier -> child to refcount 0,
// onto nullcount_map) then hfree the now-unreferenced node. This is the explicit-free
// path that used to leak stale nullcount entries.
__attribute__((noinline)) static void destroy(struct node *head) {
  while (head) {
    struct node *nx = head->next;
    head->next = NULL;  // dec barrier: child -> refcount 0 (enters nullcount_map)
    hfree(head);        // free the node, recycling its mapping slot
    head = nx;
  }
}

int main(void) {
  for (int r = 0; r < ROUNDS; r++) {
    struct node *head = build(N);
    int rc = verify(head, N);
    if (rc) {
      fprintf(stderr, "[refcount_reclaim_reuse] FAIL: verify rc=%d (round %d) -- a live "
                      "node was reclaimed and its mapping slot reused (stale nullcount "
                      "entry?).\n",
          rc, r);
      return 1;
    }
    destroy(head);
  }
  printf("[refcount_reclaim_reuse] PASS: %d rounds x %d nodes, explicit free + rebuild, "
         "no live handle reclaimed.\n",
      ROUNDS, N);
  return 0;
}

#endif  // ALASKA_ENABLE_REFCOUNT
