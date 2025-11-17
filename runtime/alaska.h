#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {

#define NOEXCEPT noexcept
#else
#define NOEXCEPT
#endif

// Allocate a handle as well as its backing memory if the runtime decides to do
// so. This is the main interface to alaska's handle system, as actually using
// handles is entirely transparent.
extern void *
halloc(size_t sz) NOEXCEPT; // __attribute__((alloc_size(1), malloc, nothrow));

// calloc a handle, returning a zeroed array of nmemb elements, each of size
// `size`
extern void *hcalloc(size_t nmemb, size_t size);

// Realloc a handle. This function will always return the original handle, as it
// just needs to reallocate the backing memory. Thus, if your application relies
// on realloc returning something different, you should be careful!
extern void *hrealloc(void *handle, size_t sz);

// Free a given handle. Is a no-op if ptr=null
extern void hfree(void *ptr);

extern size_t alaska_usable_size(void *ptr);

// "Go do something in the runtime", whatever that means in the active service.
// Most of the time, this will lead to a "slow" stop-the-world event, as the
// runtime must know all active/locked handles in the application as to avoid
// corrupting program state.
extern void alaska_barrier(void);

// Grab the current resident set size in kilobytes from the kernel
extern long alaska_translate_rss_kb(void);

// Get the current timestamp in nanoseconds. Mainly to be used
// for (end - start) time keeping and benchmarking
extern unsigned long alaska_timestamp(void);

struct alaska_blob_config {
  uintptr_t code_start, code_end;
  void *stackmap;
};
// In barrier.cpp
void alaska_blob_init(struct alaska_blob_config *cfg);

// Not a good function to call. This is always an external function in the
// compiler's eyes
extern void *__alaska_leak(void *);

// A Domain in alaska is a slice of the handle table that is
// managed in a specific way. For example, a database engine might
// have one domain which is used for disk-backed handles, and another
// for in-memory handles. It could then perform operations on all
// disk-backed handles in a single batch, while leaving the in-memory
// handles alone. Behind the scenes, domains are just a set of handle
// table slabs, and allocations using `halloc` will allocate handles
// from the default domain.
//
// the `struct alaska_domain` is opaque to users of the API.
typedef struct alaska_domain alaska_domain_t;

// The configuration structure for creating a new domain.
struct alaska_domain_config { int todo; };

// Create a new Domain. Returns NULL on failure.
// The `cfg` parameter is reserved for future use and is currently ignored.
alaska_domain_t *alaska_domain_create(struct alaska_domain_config *cfg);
void alaska_domain_destroy(alaska_domain_t *a);

// Allocate a handle from the given domain
void *alaska_domain_alloc(alaska_domain_t *a, size_t sz);

// Free a handle associated with the given domain
void alaska_domain_free(alaska_domain_t *a, void *ptr);

#ifdef __cplusplus
}
#endif
