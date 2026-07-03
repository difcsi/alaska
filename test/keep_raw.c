// Test for the keep-raw optimization, which leaves a malloc/calloc whose result
// provably does not escape its function on the libc allocator instead of
// promoting it to an Alaska handle. Keep-raw is ON by default in the compiler;
// --no-keep-raw disables it.
//
// Build it both ways with the alaska driver and run the result:
//
//   alaska -O3               test/keep_raw.c -o kr   # default: non-escaping -> raw
//   alaska -O3 --no-keep-raw test/keep_raw.c -o kr   # baseline: all -> handles
//
// In both configurations the program must print the same answer and exit 0:
// keeping a non-escaping object raw is transparent to the program, exactly as
// promotion is. (The companion event-counter check in the `keep-raw-events`
// flow additionally asserts that the default build performs fewer hallocs.)
//
// Sizes and the branch condition are read through `volatile` globals so the
// optimizer cannot constant-fold them, turn the heap allocations into stack
// allocas, or delete them before the alaska passes run -- the mallocs must
// genuinely reach the compiler for the test to mean anything.

#include <stdio.h>
#include <stdlib.h>

volatile int v100 = 100;
volatile int v50 = 50;
volatile int v10 = 10;
volatile int v4 = 4;
volatile int v_true = 1;
volatile int v_false = 0;

long *g_escape;  // an escaping allocation is published here

// Non-escaping local scratch: used only through the pointer, then freed.
// EXPECT: kept raw (does not escape, freed locally).
__attribute__((noinline)) long local_scratch(void) {
  int n = v100;
  volatile long *p = (volatile long *)malloc((size_t)n * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = i + 1;
  long s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  free((void *)p);
  return s;  // 1..100 = 5050
}

// Address stored into a global -> escapes. EXPECT: promoted to a handle.
__attribute__((noinline)) void make_escape(void) {
  int n = v4;
  long *p = (long *)malloc((size_t)n * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = 10 * (i + 1);
  g_escape = p;  // escape
}

// Two non-escaping mallocs merged by a phi feeding one local free. EXPECT (v1):
// stays a handle -- the conservative free-check rejects a free reached through a
// phi/select. Output must still be correct either way.
__attribute__((noinline)) long phi_scratch(int cond) {
  int n = cond ? v50 : (2 * v50);
  volatile long *p = cond ? (volatile long *)malloc((size_t)v50 * sizeof(long))
                          : (volatile long *)malloc((size_t)(2 * v50) * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = i + 1;
  long s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  free((void *)p);
  return s;
}

// Pointer value passed to a vararg function (printf %p) -> escapes. EXPECT:
// promoted to a handle. The formatted address is not included in the checksum
// (it differs raw vs handle and run to run), only that something was written.
__attribute__((noinline)) long vararg_escape(void) {
  int n = v10;
  volatile long *p = (volatile long *)malloc((size_t)n * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = i + 1;
  long s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  char buf[32];
  snprintf(buf, sizeof(buf), "%p", (void *)p);  // pointer escapes via vararg
  free((void *)p);
  return s + (buf[0] != 0);  // +1, deterministic
}

// Many non-escaping scratch allocations from a single call site. EXPECT: kept
// raw -- so this is where the halloc-count delta between the two builds is large.
__attribute__((noinline)) long stress(int iters) {
  long acc = 0;
  for (int i = 0; i < iters; i++) {
    int n = v10;
    volatile long *p = (volatile long *)malloc((size_t)n * sizeof(long));
    p[0] = 1;
    acc += p[0];  // always +1
    free((void *)p);
  }
  return acc;  // == iters
}

// Reads through `p` but never stores it -> `p` does not escape sum_nocapture.
// noinline keeps the call (and the malloc behind it in the caller) real.
__attribute__((noinline)) long sum_nocapture(const long *p, int n) {
  long s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  return s;
}

// malloc -> passed to a non-capturing callee -> freed locally. EXPECT (case b):
// kept raw, because sum_nocapture provably does not capture the pointer.
__attribute__((noinline)) long callee_nocapture(void) {
  int n = v10;
  long *p = (long *)malloc((size_t)n * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = i + 1;
  long s = sum_nocapture(p, n);  // p does not escape sum_nocapture
  free(p);
  return s;  // 1..10 = 55
}

// malloc -> grown by realloc -> used -> freed, none escaping. EXPECT (realloc
// chain): the whole malloc/realloc/free chain kept raw.
__attribute__((noinline)) long realloc_chain(void) {
  int n = v10;
  volatile long *p = (volatile long *)malloc((size_t)n * sizeof(long));
  for (int i = 0; i < n; i++) p[i] = i + 1;
  int n2 = 2 * n;
  p = (volatile long *)realloc((void *)p, (size_t)n2 * sizeof(long));
  for (int i = n; i < n2; i++) p[i] = i + 1;
  long s = 0;
  for (int i = 0; i < n2; i++) s += p[i];
  free((void *)p);
  return s;  // 1..20 = 210
}

int main(void) {
  long a = local_scratch();  // 5050
  make_escape();             // g_escape = {10,20,30,40}
  long b = g_escape[0] + g_escape[1] + g_escape[2] + g_escape[3];  // 100
  long c = phi_scratch(v_true);   // 1275
  long d = phi_scratch(v_false);  // 5050
  long e = vararg_escape();       // 56
  long f = stress(1000);          // 1000
  long g = callee_nocapture();    // 55  (case b)
  long h = realloc_chain();       // 210 (realloc chain)
  free(g_escape);

  long total = a + b + c + d + e + f + g + h;  // 12796
  printf("total=%ld (expect 12796)\n", total);
  if (total != 12796) {
    printf("FAIL\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
