// Regression test for huge-object dec-on-free (alaska_hfree_dec_children, see
// runtime/rt/refcount.cpp).
//
// THE BUG: dec-on-free keyed its object base + size on the handle's Mapping, so a HUGE
// object (>= the huge-object threshold, which has no Mapping -- the value halloc returns
// IS the raw backing pointer) fell through and its fields were never scanned. But the
// inc barrier still fires on handle stores INTO a huge object, so a huge container full
// of handles inc'd its children on fill and never dec'd them on free -- an unbounded
// refcount leak that kept those children (and everything they reach) from ever being
// reclaimed.
//
// THE FIX: derive the scan base from the raw pointer when there is no Mapping, so a huge
// object's handle-typed fields are decremented on free like any other aggregate.
//
// THE SHAPE THAT TRIGGERS IT: allocate a HUGE array of handles (so it has no Mapping),
// fill it with handles to child objects that are ALSO kept alive by a second, untouched
// array. Each child therefore holds two heap references (refcount delta of exactly +2
// over its birth count). Freeing the huge array must drop each child's refcount by
// exactly one; pre-fix it dropped by zero. The children stay live via the keepalive
// array, so reading their refcounts back is safe.
//
// Self-checks and exits non-zero on any child whose refcount did not fall by one.

#include <alaska.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !ALASKA_ENABLE_REFCOUNT

int main(void) {
  printf("[refcount_huge_children] ALASKA_ENABLE_REFCOUNT is off; skipping.\n");
  return 0;
}

#else

// 16384 handles * 8 bytes = 128 KiB, comfortably above the huge-object threshold
// (0xFFFF = 64 KiB-ish), so the array is allocated as a huge object (no Mapping).
#define N 16384

// A huge object is returned as its raw backing pointer (top bit clear -> non-negative),
// whereas a sized handle has the top (handle) bit set (negative). Used to assert the
// array really landed on the huge path -- if not, the test isn't exercising the fix.
static int is_huge_ptr(void *p) { return (intptr_t)p >= 0; }

int main(void) {
  void **huge = (void **)halloc(N * sizeof(void *));  // huge: array of handles
  void **keep = (void **)halloc(N * sizeof(void *));   // keepalive: holds the children live
  if (!huge || !keep) {
    fprintf(stderr, "[refcount_huge_children] halloc failed\n");
    return 1;
  }
  if (!is_huge_ptr(huge)) {
    fprintf(stderr, "[refcount_huge_children] array did not land on the huge path "
                    "(increase N); test not meaningful.\n");
    return 2;
  }
  // Start from clean slots so the first handle store dec's a NULL (no-op) rather than a
  // garbage word that might look like a handle.
  memset(huge, 0, N * sizeof(void *));
  memset(keep, 0, N * sizeof(void *));

  unsigned long rc_before[N];
  for (long i = 0; i < N; i++) {
    void *child = halloc(sizeof(long));
    if (!child) {
      fprintf(stderr, "[refcount_huge_children] child halloc failed at %ld\n", i);
      return 1;
    }
    keep[i] = child;  // heap store -> inc child
    huge[i] = child;  // heap store -> inc child (this is the reference under test)
    rc_before[i] = alaska_get_refcount(child);
    if (rc_before[i] < 1) {
      fprintf(stderr, "[refcount_huge_children] unexpected child refcount %lu at %ld\n",
          rc_before[i], i);
      return 3;
    }
  }

  // Free the huge array. With the fix this walks its words and decrements each child;
  // huge objects bypass the deferred queue and free inline, so the decrements have
  // landed by the time hfree returns.
  hfree(huge);

  long bad = 0;
  for (long i = 0; i < N; i++) {
    unsigned long rc_after = alaska_get_refcount(keep[i]);
    if (rc_after != rc_before[i] - 1) {
      if (bad < 5) {
        fprintf(stderr, "[refcount_huge_children] child %ld: refcount %lu -> %lu "
                        "(expected -1); huge object's field was not dec'd on free.\n",
            i, rc_before[i], rc_after);
      }
      bad++;
    }
  }
  if (bad) {
    fprintf(stderr, "[refcount_huge_children] FAIL: %ld/%d children not decremented.\n", bad, N);
    return 1;
  }

  printf("[refcount_huge_children] PASS: %d children each dropped exactly one ref when "
         "their huge container was freed.\n",
      N);
  return 0;
}

#endif  // ALASKA_ENABLE_REFCOUNT
