/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2023, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2023, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <alaska/ThreadCache.hpp>
#include <alaska/Runtime.hpp>
#include "alaska/liballoc.h"
#include <assert.h>
#include <sys/signal.h>

// This library uses the alaska memory allocator, but *does not* return handles. Instead, it returns
// pointers to the allocated memory. This is done to create a fair baseline which rules out the
// alaska allocator's performance as a source of overhead. This is just to get a good idea of the
// performance impact that raw handles have real applications.


#define write_csr(reg, val) \
  ({ asm volatile("csrw %0, %1" ::"i"(reg), "rK"((uint64_t)val) : "memory"); })


#define read_csr(csr, val) \
  __asm__ __volatile__("csrr %0, %1" : "=r"(val) : "n"(csr) : /* clobbers: none */);


static uint64_t instructions_in_runtime = 0;
static thread_local alaska::ThreadCache *tc = NULL;
static alaska::Runtime *the_runtime = NULL;
static char stdout_buf[BUFSIZ];
static char stderr_buf[BUFSIZ];


static volatile bool delayed_localization_pending = false;
static volatile long localization_latch_depth = 0;
static long localizations = 0;
static alaska::RateCounter immediate_localizations;
static alaska::RateCounter delayed_localizations;


static void yukon_signal_handler(int sig, siginfo_t *info, void *ucontext) {
  // If a pagefault occurs while handle table walking, we will throw the
  // exception back up and even if you handle the page fault, the HTLB
  // stores the fact that it will cause an exception until you
  // invalidate the entry.
  if (sig == SIGSEGV) {
    // TODO: if the faulting address has the top bit set  (sv39) then we need to
    //       treat that as a page fault to the *handle table*. Basically, we need
    //       to read/write that handle entry.
    // printf("Caught segfault to address %p. Clearing htlb and trying again!\n", info->si_addr);
#if defined(__riscv)
    ucontext_t *uc = (ucontext_t *)ucontext;
    greg_t *regs = uc->uc_mcontext.__gregs;

    printf("Segfault at address: %p on thread %ld\n", info->si_addr, pthread_self());
    printf("Register state:\n");
    printf("ra =0x%016lx inst=%08x\n", regs[0], *(uint32_t *)regs[0]);
    printf("sp =0x%016lx\n", regs[1]);
    printf("gp =0x%016lx\n", regs[2]);
    printf("tp =0x%016lx\n", regs[3]);
    for (int i = 4; i < 32; i++) {
      printf("x%-2d=0x%016lx\n", i, regs[i]);
    }
#endif

    exit(-11);
    // write_csr(CSR_HTINVAL, ((1LU << (64 - ALASKA_SIZE_BITS)) - 1));
    return;
  }
}


static inline uint64_t read_instret() {
  uint64_t instret;
  asm volatile("rdinstret %0" : "=r"(instret));
  return instret;
}



// These two classe (AutoFencer and LocalizationLatch) are not
// actually needed in the stub.  But they are here to make the stub
// more similar to the real yukon runtime.
struct AutoFencer {
  uint64_t start_inst;
  AutoFencer() { start_inst = read_instret(); }
  ~AutoFencer() {
    __asm__ volatile("fence" ::: "memory");
    instructions_in_runtime += (read_instret() - start_inst);
  }
};


struct LocalizationLatch {
  LocalizationLatch() { localization_latch_depth++; }
  ~LocalizationLatch() { localization_latch_depth--; }
};



__attribute__((noinline)) void init(void) {
  setvbuf(stdout, stdout_buf, _IOLBF, BUFSIZ);
  setvbuf(stderr, stderr_buf, _IOLBF, BUFSIZ);

  alaska::Configuration config;
  config.huge_strategy = alaska::HugeAllocationStrategy::MALLOC_BACKED;

  the_runtime = new alaska::Runtime(config);
  // void *handle_table_base = the_runtime->handle_table.get_base();
  // printf("Handle table at %p\n", handle_table_base);
  // Make sure the handle table performs mlocks
  the_runtime->handle_table.enable_mlock();

  // struct sigaction sa;
  // memset(&sa, 0, sizeof(sa));
  // // Block signals while we are in these signal handlers.
  // sigemptyset(&sa.sa_mask);
  // sigaddset(&sa.sa_mask, SIGUSR2);
  // // Store siginfo (ucontext) on the stack of the signal
  // // handlers (so we can grab the return address)
  // sa.sa_flags = SA_SIGINFO;
  // // Go to the `barrier_signal_handler`
  // sa.sa_sigaction = yukon_signal_handler;
  // // Attach this action on two signals:
  // assert(sigaction(SIGSEGV, &sa, NULL) == 0);


  atexit([]() {
    alaska::printf("YUKON_RTINST=%zu\n", instructions_in_runtime);
  });
}


__attribute__((noinline))
static void thread_cache_init(void) {
  if (the_runtime == NULL) init();
  if (tc == NULL) tc = the_runtime->new_threadcache();
}

static inline alaska::ThreadCache *get_tc() {
  if (unlikely(tc == NULL)) thread_cache_init();
  return tc;
}



void __attribute__((constructor(102))) alaska_init(void) {
  unsetenv("LD_PRELOAD");  // make it so we don't run alaska in subprocesses!
  // allocate
  get_tc();
}



static void *from_handle(void *ptr) {
  auto *m = alaska::Mapping::from_handle_safe(ptr);
  if (m == nullptr) return ptr;

  // alaska::printf("from %p -> %p\n", ptr, m->get_pointer());
  return m->get_pointer();
}

static void *reverse(void *ptr) {
  if (ptr == NULL) return NULL;
  if (alaska_internal_is_managed(ptr)) return ptr;
  auto header = alaska::ObjectHeader::from(ptr);
  // alaska::printf("revr %p -> %p\n", ptr, header->get_mapping()->to_handle());
  return header->get_mapping()->to_handle(0);
}


static void *_halloc(size_t sz, int zero) {
  void *result = NULL;

  result = get_tc()->halloc(sz, zero);
  if (result == NULL) errno = ENOMEM;
  return from_handle(result);
}

extern "C" void *halloc(size_t sz) noexcept {
  LocalizationLatch loc_latch;
  // AutoFencer fencer;
  return _halloc(sz, 0);
}
extern "C" void *hcalloc(size_t nmemb, size_t size) {
  LocalizationLatch loc_latch;
  // AutoFencer fencer;
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
extern "C" void *hrealloc(void *handle, size_t new_size) {
  LocalizationLatch loc_latch;
  // AutoFencer fencer;

  alaska::LockedThreadCache tc = *get_tc();

  // If the handle is null, then this call is equivalent to malloc(size)
  if (handle == NULL) {
    return malloc(new_size);
  }


  handle = reverse(handle);

  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m == NULL) {
    if (!alaska::Runtime::get().heap.huge_allocator.owns(handle)) {
      log_fatal("realloc edge case: not a handle %p!", handle);
      exit(-1);
    }
  }

  // If the size is equal to zero, and the handle is not null, realloc acts like free(handle)
  if (new_size == 0) {
    log_debug("realloc edge case: zero size %p!", handle);
    // If it wasn't a handle, just forward to the system realloc
    free(handle);
    return NULL;
  }

  return from_handle(tc->hrealloc(handle, new_size));
}



extern "C" void hfree(void *ptr) {
  LocalizationLatch loc_latch;
  // AutoFencer fencer;
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;
  ptr = reverse(ptr);


  alaska::LockedThreadCache tc = *get_tc();
  tc->hfree(ptr);
}


extern "C" size_t halloc_usable_size(void *ptr) {
  LocalizationLatch loc_latch;
  ptr = reverse(ptr);
  alaska::LockedThreadCache tc = *get_tc();
  return tc->get_size(ptr);
}



void *operator new(size_t size) { return halloc(size); }
void *operator new[](size_t size) { return halloc(size); }
void operator delete(void *ptr) { hfree(ptr); }
void operator delete[](void *ptr) { hfree(ptr); }


extern "C" {
void *malloc(size_t size) { return halloc(size); }
void *calloc(size_t size, size_t count) { return hcalloc(size, count); }
void *realloc(void *ptr, size_t newsize) { return hrealloc(ptr, newsize); }
void free(void *ptr) { hfree(ptr); }
size_t malloc_usable_size(void *ptr) { return halloc_usable_size(ptr); }
}
