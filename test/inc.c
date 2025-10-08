#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <yukon.h>

#define managed __attribute__((address_space(1)))
#define COUNT 10000000

__attribute__((noinline)) static void inc(volatile int *x) {
  *x += 1;
}

int main(int argc, char **argv) {
  for (int i = 0; i < 1 << 21; i++) {
    int *p = malloc(sizeof(*p));
    *p = 4;
    printf("handle = 0x%zx, esc = %p\n", (uintptr_t)p, p);
    inc(p);
    // free(p);
  }
  return 0;
}
