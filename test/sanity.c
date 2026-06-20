#include <stdio.h>
#include <alaska.h>

// Reference counting is an optional, configure-time feature (ALASKA_ENABLE_REFCOUNT).
// When it is compiled out, alaska_get_refcount() does not exist, so fall back to a
// stub that reports 0 and keeps this sanity test buildable in both configurations.
#if ALASKA_ENABLE_REFCOUNT
#define SANITY_REFCOUNT(p) alaska_get_refcount((void *)(p))
#else
#define SANITY_REFCOUNT(p) ((unsigned long)0)
#endif

int main() {
  int *x = (int *)halloc(sizeof(int));
  printf("&x=0x%zx, refcount=%lu\n", (uintptr_t)x, SANITY_REFCOUNT(x));

  printf("Important bit\n");
  int** y = halloc(sizeof(int*));
  printf("alloc'd\n");
  *y = x;
  int* z;
  z = x;
  printf("Created alias y -> x\n");
  printf("y=%p, *y=%p\n", (void*)y, (void*)*y);  // Force compiler to keep the store
  // z is an int* alias of x, so *z is an int -- print it as one. Casting *z to
  // void* sign-extends the (here uninitialized) int into a value whose top bit
  // may be set; the escape-translation pass then treats it as a handle and
  // dereferences garbage (intermittent SIGSEGV). Print the int directly.
  printf("z=%p, *z=%d\n", (void*)z, *z);  // Force compiler to keep the load
  printf("After creating alias y: refcount=%lu\n", SANITY_REFCOUNT(x));
  
  printf("Before realloc: x=0x%zx, refcount=%lu\n", (uintptr_t)x, SANITY_REFCOUNT(x));
	x = (int *)hrealloc((void*)x, sizeof(int) * 400);
  printf("After realloc: x=0x%zx, refcount=%lu\n", (uintptr_t)x, SANITY_REFCOUNT(x));
  
  *x = 42;
  printf("Wrote 42 to x. x=0x%zx, refcount=%lu\n", (uintptr_t)x, SANITY_REFCOUNT(x));
  //*y = x;
  **y = 41;
  printf("Wrote 41 to *y\n");
  printf("It works! x=%d, x=0x%zx, refcount=%lu\n", *x, (uintptr_t)x, SANITY_REFCOUNT(x));
  *y = 0x0;
  printf("removed alias y -> x: x=0x%zx, refcount=%lu\n", (uintptr_t)x, SANITY_REFCOUNT(x));
  
  
  printf("Before free: refcount=%lu\n", SANITY_REFCOUNT(x));
	hfree((void*)x);
  printf("After free: refcount=%lu\n", SANITY_REFCOUNT(x));
  
  return 0;
}
