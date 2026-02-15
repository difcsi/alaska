#include <stdio.h>
#include <alaska.h>

int main() {
  int *x = (int *)halloc(sizeof(int));
  printf("&x=0x%zx, refcount=%lu\n", (uintptr_t)x, alaska_get_refcount((void*)x));

  printf("Important bit\n");
  int** y = halloc(sizeof(int*));
  printf("alloc'd\n");
  *y = x;
  int* z;
  z = x;
  printf("Created alias y -> x\n");
  printf("y=%p, *y=%p\n", (void*)y, (void*)*y);  // Force compiler to keep the store
  printf("z=%p, *z=%p\n", (void*)z, (void*)*z);  // Force compiler to keep the store
  printf("After creating alias y: refcount=%lu\n", alaska_get_refcount((void*)x));
  
  printf("Before realloc: x=0x%zx, refcount=%lu\n", (uintptr_t)x, alaska_get_refcount((void*)x));
	x = (int *)hrealloc((void*)x, sizeof(int) * 400);
  printf("After realloc: x=0x%zx, refcount=%lu\n", (uintptr_t)x, alaska_get_refcount((void*)x));
  
  *x = 42;
  printf("Wrote 42 to x. x=0x%zx, refcount=%lu\n", (uintptr_t)x, alaska_get_refcount((void*)x));
  //*y = x;
  **y = 41;
  printf("Wrote 41 to *y\n");
  printf("It works! x=%d, x=0x%zx, refcount=%lu\n", *x, (uintptr_t)x, alaska_get_refcount((void*)x));
  *y = 0x0;
  printf("removed alias y -> x: x=0x%zx, refcount=%lu\n", (uintptr_t)x, alaska_get_refcount((void*)x));
  
  
  printf("Before free: refcount=%lu\n", alaska_get_refcount((void*)x));
	hfree((void*)x);
  printf("After free: refcount=%lu\n", alaska_get_refcount((void*)x));
  
  return 0;
}
