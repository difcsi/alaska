// Test for the "Yukon" stack-to-heap promotion pass (alaska-stack-promote).
//
// Build it three ways with the alaska driver and run the result:
//
//   alaska -O3                      test/heap_hints.c -o hh   # no promotion
//   alaska -O3 --stack-promote      test/heap_hints.c -o hh   # promote all
//   alaska -O3 --heap-hints F.txt   test/heap_hints.c -o hh   # promote hinted only
//
// In every configuration the program must print the same answers and exit 0.
// The point is that once an address-taken local is rewritten into a `halloc`
// handle, every access to it still goes through Alaska's translation machinery
// and observes the same values -- so promotion is transparent to the program.

#include <stdio.h>

typedef struct {
  int x, y;
} Point;

// `mirror` is listed in the heap-hints file: its Point* argument is the one that
// forces promotion of `a` below. noinline keeps the address genuinely escaping
// (so -O3 cannot SROA the local away before the promotion pass runs).
__attribute__((noinline)) void mirror(Point *p) {
  int t = p->x;
  p->x = p->y;
  p->y = t;
}

// Sums an array through a pointer the caller takes the address of.
__attribute__((noinline)) long sum(const long *a, int n) {
  long s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

__attribute__((noinline)) int run(void) {
  Point a = {2, 3};
  mirror(&a);  // a's address escapes into mirror -> promoted to a handle

  long buf[4] = {10, 20, 30, 40};
  long total = sum(buf, 4);  // buf's address escapes into sum

  // mirror swapped a -> {3, 2}; sum(buf) = 100.
  return a.x * 1000 + a.y * 100 + (int)total;  // expect 3*1000 + 2*100 + 100 = 3300
}

int main(void) {
  int r = run();
  printf("result=%d (expect 3300)\n", r);
  if (r != 3300) {
    printf("FAIL\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
