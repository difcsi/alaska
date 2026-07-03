// Integration test for the compiler's reference-count barrier *elisions*
// (compiler/passes/RefcountInc.cpp + RefcountDec.cpp, helpers in
// compiler/include/alaska/RefcountElision.h):
//
//   Tier 1 -- the INCREMENT barrier is elided when the stored value provably is
//             not a live handle (a global/function address, null, or a small
//             non-negative inttoptr constant). Such an inc is a runtime no-op, so
//             eliding it must not change behaviour.
//   Tier 2 -- a redundant self-copy `*P = *P` has its inc AND its matching dec
//             elided together (they cancel).
//
// Unlike refcount_collect.c, this test relies on the COMPILER-inserted store
// barriers (real pointer stores below), because those are exactly what the elision
// passes rewrite. The elisions are semantics-preserving, so the test is a SOUNDNESS
// gate: it cannot observe whether an elision fired, but a *wrong* elision shows up
// here as corruption, a premature free, a leak, or a crash.
//
// The danger we hunt is a wrongly-elided REAL increment: every node mixes one real
// child handle (whose inc must be KEPT) with several non-handle fields (whose inc
// Tier 1 elides). If the child's inc were dropped, that node's refcount would be one
// too low. We expose that by keeping the whole structure LIVE -- correctly-counted
// interior nodes sit at refcount 1, so the background collector must never free them
// -- and walking it repeatedly while the collector runs. An under-counted node falls
// to refcount 0, gets reclaimed out from under us, and the walk then sees a wrong id,
// a broken link, or faults.
//
// We keep everything alive for the whole run (no hfree, no dropping references, so no
// reclamation churn). That keeps the test independent of an unrelated, pre-existing
// reclamation fault in the refcount-gc config that mass hfree+rebuild can trip.

#include <alaska.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !ALASKA_ENABLE_REFCOUNT

// The elision passes only run in refcount builds. Nothing to test otherwise; report
// and succeed so the target stays green in every configuration.
int main(void) {
  printf("[refcount_elision] ALASKA_ENABLE_REFCOUNT is off; skipping.\n");
  return 0;
}

#else

// Globals whose ADDRESSES are non-handles: storing them into a heap field is a
// Tier 1 inc-elision site. Their stored values must survive intact.
static int g_int = 0xA5;
static const char g_str[] = "alaska";
static void marker(void) {}

#define LISTS 12             // independent lists, all kept alive together
#define N 20000              // nodes per list (spans many handle slabs)
#define SETTLE_MS 1200       // walk-and-wait budget; collector cycles ~every 50-250ms
#define TAG ((void *)0x2A)   // small positive inttoptr constant -> non-handle

struct node {
  struct node *next;  // a REAL handle (the prior node) -> inc MUST be kept
  void *fn;           // function address (global)      -> Tier 1 elides inc
  void *glob;         // global data address            -> Tier 1 elides inc
  void *nul;          // null                           -> Tier 1 elides inc
  void *tag;          // inttoptr small positive const  -> Tier 1 elides inc
  long id;
};

// Build a singly linked list of `n` nodes. `p->next = head` stores a real handle
// (inc kept); the four pointer fields store non-handles (Tier 1 inc-elision sites).
__attribute__((noinline)) static struct node *build(long n) {
  struct node *head = NULL;
  for (long i = 0; i < n; i++) {
    struct node *p = (struct node *)halloc(sizeof *p);
    if (!p) {
      fprintf(stderr, "[refcount_elision] halloc failed at %ld\n", i);
      exit(1);
    }
    p->next = head;            // real handle -> inc kept
    p->fn = (void *)marker;    // non-handle
    p->glob = (void *)&g_int;  // non-handle
    p->nul = (void *)0;        // non-handle
    p->tag = TAG;              // non-handle
    p->id = i;
    head = p;
  }
  return head;
}

// Reassign every field to itself: `p->next = p->next` is a real-handle self-copy
// (Tier 2 must drop inc and dec together; dropping only one would leak or free
// early), and the non-handle self-copies must likewise stay correct. Most of these
// are removed by clang's own DSE before Alaska runs, so this mainly guards the
// cases that survive -- but it must never corrupt the list.
__attribute__((noinline)) static void self_copy(struct node *head) {
  for (struct node *p = head; p; p = p->next) {
    p->next = p->next;
    p->fn = p->fn;
    p->glob = p->glob;
    p->tag = p->tag;
  }
}

// Walk the list and check length, ids, child links, and every non-handle field. A
// premature free of any node tends to surface as a wrong id, a broken link, or a
// fault on the dereference.
__attribute__((noinline)) static int verify(struct node *head, long n) {
  long count = 0;
  for (struct node *p = head; p; p = p->next) {
    if (p->id != n - 1 - count) return 1;
    if (p->fn != (void *)marker) return 2;
    if (p->glob != (void *)&g_int) return 3;
    if (p->nul != (void *)0) return 4;
    if (p->tag != TAG) return 5;
    count++;
  }
  return (count == n) ? 0 : 6;
}

static unsigned long now_ms(void) { return alaska_timestamp() / 1000000UL; }

int main(void) {
  // `heads` is a stack array: storing a head into it is a stack store (not refcount
  // tracked) but pins the head against collection via the conservative stack scan.
  // Every interior node is held at refcount 1 by its predecessor's `next`. So the
  // whole structure is live and the collector must not free any of it.
  struct node *heads[LISTS];

  for (int l = 0; l < LISTS; l++) {
    heads[l] = build(N);
    int rc = verify(heads[l], N);
    if (rc) {
      fprintf(stderr, "[refcount_elision] FAIL: post-build verify rc=%d (list %d)\n", rc, l);
      return 1;
    }
    self_copy(heads[l]);
  }

  // Hold everything live and keep walking it while the background collector runs. If
  // a real child inc was wrongly elided, that node is under-counted, gets reclaimed,
  // and a walk here trips. We loop for SETTLE_MS so the collector gets many cycles.
  unsigned long start = now_ms();
  long passes = 0;
  do {
    for (int l = 0; l < LISTS; l++) {
      int rc = verify(heads[l], N);
      if (rc) {
        fprintf(stderr, "[refcount_elision] FAIL: live-hold verify rc=%d (list %d, pass %ld) -- "
                        "a live node was freed early (a real inc was wrongly elided?).\n",
            rc, l, passes);
        return 1;
      }
    }
    passes++;
  } while (now_ms() - start < SETTLE_MS);

  printf("[refcount_elision] PASS: %d lists x %d nodes held live across %ld verify "
         "passes, mixed handle/non-handle stores + self-copies, integrity held (%s).\n",
      LISTS, N, passes, g_str);
  return 0;
}

#endif  // ALASKA_ENABLE_REFCOUNT
