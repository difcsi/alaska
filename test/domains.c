
#include <stdio.h>
#include <unistd.h>
#include <alaska.h>
#include <inttypes.h>

int main() {
  alaska_domain_t *d1 = alaska_domain_create(0);
  alaska_domain_t *d2 = alaska_domain_create(0);

  printf("Created domains %p and %p\n", d1, d2);

  void *h1 = alaska_domain_alloc(d1, 32);
  void *h2 = alaska_domain_alloc(d2, 32);

  printf("Allocated handles %zx and %zx\n", (uintptr_t)h1, (uintptr_t)h2);

  alaska_domain_destroy(d1);
  alaska_domain_destroy(d2);

  return 0;
}
