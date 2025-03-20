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


#include <dlfcn.h>
#include <alaska/alaska.hpp>
#include <alaska/utils.h>
#include <alaska/Runtime.hpp>
#include <alaska/Configuration.hpp>
#include "alaska/HugeObjectAllocator.hpp"
#include "alaska/ThreadCache.hpp"
#include <alaska/liballoc.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>
#include <assert.h>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>

#include <yukon/yukon.hpp>


#define write_csr(reg, val) \
  ({ asm volatile("csrw %0, %1" ::"i"(reg), "rK"((uint64_t)val) : "memory"); })


#define read_csr(csr, val) \
  __asm__ __volatile__("csrr %0, %1" : "=r"(val) : "n"(csr) : /* clobbers: none */);


static inline uint64_t read_instret() {
  uint64_t instret;
  asm volatile("rdinstret %0" : "=r"(instret));
  return instret;
}

static uint64_t instructions_in_runtime = 0;

struct AutoFencer {
  uint64_t start_inst;
  AutoFencer() { start_inst = read_instret(); }
  ~AutoFencer() {
    __asm__ volatile("fence" ::: "memory");
    instructions_in_runtime += (read_instret() - start_inst);
  }
};



#define wait_for_csr_zero(reg)         \
  do {                                 \
    volatile uint32_t csr_value = 0x1; \
    do {                               \
      read_csr(reg, csr_value);        \
    } while (csr_value != 0);          \
  } while (0);


#define CSR_HTBASE 0xc2
#define CSR_HTDUMP 0xc3
#define CSR_HTINVAL 0xc4


static thread_local alaska::ThreadCache *tc = NULL;
static alaska::ThreadCache *dump_tc = NULL;
static alaska::Runtime *the_runtime = NULL;
static uint64_t yukon_dump_interval = 10000;  // microseconds


static void yukon_signal_handler(int sig, siginfo_t *info, void *ucontext) {
  AutoFencer fencer;
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

    printf("Segfault at address: %p\n", info->si_addr);
    printf("Register state:\n");
    printf("ra =0x%016lx inst=%08x\n", regs[0], *(uint32_t *)regs[0]);
    printf("sp =0x%016lx\n", regs[1]);
    printf("gp =0x%016lx\n", regs[2]);
    printf("tp =0x%016lx\n", regs[3]);
    for (int i = 4; i < 32; i++) {
      printf("x%-2d=0x%016lx\n", i, regs[i]);
    }
#endif

    // write_csr(CSR_HTINVAL, ((1LU << (64 - ALASKA_SIZE_BITS)) - 1));
    return;
  }


  // Pause requested.
  if (sig == SIGUSR2) {
    return;
  }
}



void alarm_handler(int sig) {
  // Grab the thread cache
  auto *tc = yukon::get_dump_tc();
  // And trigger an HTLB dump
  yukon::dump_htlb(tc);

  if ((long)ualarm(yukon_dump_interval, 0) == -1) {
    perror("Failed to setup ualarm for dumping");
    exit(-1);
  }
}


static void segv_handler(int sig, siginfo_t *info, void *ucontext) {}

pthread_t dump_thread;

static void setup_signal_handlers(void) {
  unsetenv("LD_PRELOAD");
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Block signals while we are in these signal handlers.
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGUSR2);

  // Store siginfo (ucontext) on the stack of the signal
  // handlers (so we can grab the return address)
  sa.sa_flags = SA_SIGINFO;
  // Go to the `barrier_signal_handler`
  sa.sa_sigaction = yukon_signal_handler;
  // Attach this action on two signals:
  // assert(sigaction(SIGSEGV, &sa, NULL) == 0);
  assert(sigaction(SIGUSR2, &sa, NULL) == 0);
}



#define ALASKA_THREAD_TRACK_STATE_T int
#define ALASKA_THREAD_TRACK_INIT setup_signal_handlers();
#include <alaska/thread_tracking.in.hpp>



static inline uint64_t read_cycle_counter() {
  uint64_t cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}


static alaska::handle_id_t dump_buffer[1024];
static int alaska_fd = -1;

static unsigned long last_dump_cycle = 0;

namespace yukon {
  void set_handle_table_base(void *addr) {
    uint64_t value;
    read_csr(CSR_HTBASE, value);
    if (getenv("YUKON_PHYS") != nullptr) {
      value |= (1LU << 63);
    }

    write_csr(CSR_HTBASE, value);
  }




  void dump_htlb(alaska::ThreadCache *tc) {
    if (alaska_fd == -1) {
      alaska_fd = open("/dev/alaska", O_RDONLY);
    }

    // The size of the HTLB
    size_t size = 16 + 512;

    // memset(dump_buffer, 0, sizeof(dump_buffer));
    auto *space = tc->localizer.get_hotness_buffer(size);
    // auto *space = dump_buffer;

    // memset(space, 0, size * sizeof(alaska::handle_id_t));
    // Fence after we have a valid space for the dump
    asm volatile("fence" ::: "memory");

    auto start = read_cycle_counter();
    int r = read(alaska_fd, space, size * sizeof(alaska::handle_id_t));
    auto end = read_cycle_counter();




    auto localize_start = read_cycle_counter();
    // Feed the buffer to the localizer to do it's work
    tc->localizer.feed_hotness_buffer(size, space);
    auto localize_end = read_cycle_counter();


    // we assume a cycle is 1 nanosecond
    double total_time_ms = (localize_end - start) / 1000000.0l;

    printf("[localizer cycles] dump:%20lu    between:%20lu   localize:%20lu (total: %.3fms)\n", end - start,
           start - last_dump_cycle, localize_end - localize_start, total_time_ms);
    last_dump_cycle = localize_end;

    // Fence again... for some reason
    asm volatile("fence" ::: "memory");
  }


  void print_htlb(void) {
    size_t size = 576;
    uint64_t handle_ids[size];
    memset(handle_ids, 0, size * sizeof(alaska::handle_id_t));

    asm volatile("fence" ::: "memory");
    write_csr(CSR_HTDUMP, (uint64_t)handle_ids);
    wait_for_csr_zero(CSR_HTDUMP);
    asm volatile("fence" ::: "memory");

    printf("========================\n");

    int cols = 0;
    for (size_t i = 0; i < size; i++) {
      printf("%16lx ", handle_ids[i]);
      cols++;
      if (cols >= 8) {
        cols = 0;
        printf("\n");
      }
    }
    if (cols != 0) {
      printf("\n");
    }
  }

  alaska::ThreadCache *get_tc() {
    if (the_runtime == NULL) init();
    if (tc == NULL) tc = the_runtime->new_threadcache();
    return tc;
  }

  alaska::ThreadCache *get_dump_tc() {
    if (the_runtime == NULL) init();
    if (dump_tc == NULL) dump_tc = the_runtime->new_threadcache();
    return dump_tc;
  }


  static char stdout_buf[BUFSIZ];
  static char stderr_buf[BUFSIZ];
  void init(void) {
    setvbuf(stdout, stdout_buf, _IOLBF, BUFSIZ);
    setvbuf(stderr, stderr_buf, _IOLBF, BUFSIZ);

    alaska::Configuration config;
    config.huge_strategy = alaska::HugeAllocationStrategy::MALLOC_BACKED;

    the_runtime = new alaska::Runtime(config);
    // void *handle_table_base = the_runtime->handle_table.get_base();
    // printf("Handle table at %p\n", handle_table_base);
    // Make sure the handle table performs mlocks
    the_runtime->handle_table.enable_mlock();


    if (getenv("YUKON_PHYS") != nullptr) {
      uint64_t value;
      read_csr(CSR_HTBASE, value);
      value |= (1LU << 63);
      write_csr(CSR_HTBASE, value);
    }



    if (getenv("YUKON_DUMP") != NULL) {
      int dump_interval_ms = 10;

      // pthread_create(
      //     &dump_thread, NULL,
      //     [](void *) -> void * {
      //       // Grab the thread cache
      //       auto *tc = yukon::get_tc();

      //       while (1) {
      //         usleep(20 * 1000);
      //         // And trigger an HTLB dump
      //         yukon::dump_htlb(tc);
      //       }
      //       return NULL;
      //     },
      //     NULL);

      printf("dumping every %d ms\n", dump_interval_ms);
      yukon_dump_interval = dump_interval_ms * 1000;
      signal(SIGALRM, alarm_handler);
      // now that we have sigalarm configured, setup a ualarm for
      // some number of microseconds on an interval for dumping
      if ((long)ualarm(yukon_dump_interval * 10, 0) == -1) {
        perror("Failed to setup ualarm for dumping");
        exit(-1);
      }
    }


    // asm volatile("fence" ::: "memory");
    // yukon::set_handle_table_base(handle_table_base);
    // asm volatile("fence" ::: "memory");
  }
}  // namespace yukon



void __attribute__((constructor(102))) alaska_init(void) {
  unsetenv("LD_PRELOAD");  // make it so we don't run alaska in subprocesses!
  // allocate
  yukon::get_tc();
}

void __attribute__((destructor)) alaska_deinit(void) {
  printf("YUKON_RTINST=%zu\n", instructions_in_runtime);
}


static void touch_pages(uintptr_t start, uintptr_t end) {
  constexpr uintptr_t page_size = 4096;
  // Align the start to the beginning of the first 4KB page in the range
  uintptr_t current_page = start & ~(page_size - 1);
  if (current_page < start) {
    current_page += page_size;
  }

  // Loop through each page until we exceed the end
  while (current_page < end) {
    volatile uint8_t *p = (volatile uint8_t *)current_page;
    *p = 0;
    // Move to the next page
    current_page += page_size;
  }
}

static void *_halloc(size_t sz, int zero) {
  void *result = NULL;

  result = yukon::get_tc()->halloc(sz, zero);

  auto m = alaska::Mapping::from_handle_safe(result);
  // if (m) {
  //   auto backing_data = (uintptr_t)m->get_pointer();
  //   touch_pages(backing_data, backing_data + sz);
  // }
  if (result == NULL) errno = ENOMEM;

  return result;
}

extern "C" void *halloc(size_t sz) noexcept {
  AutoFencer fencer;
  return _halloc(sz, 0);
}
extern "C" void *hcalloc(size_t nmemb, size_t size) {
  AutoFencer fencer;
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
extern "C" void *hrealloc(void *handle, size_t new_size) {
  AutoFencer fencer;

  alaska::LockedThreadCache tc = *yukon::get_tc();

  // If the handle is null, then this call is equivalent to malloc(size)
  if (handle == NULL) {
    return malloc(new_size);
  }
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

  handle = tc->hrealloc(handle, new_size);
  return handle;
}



extern "C" void hfree(void *ptr) {
  AutoFencer fencer;
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;


  alaska::LockedThreadCache tc = *yukon::get_tc();
  tc->hfree(ptr);
}


extern "C" size_t halloc_usable_size(void *ptr) {
  alaska::LockedThreadCache tc = *yukon::get_tc();
  return tc->get_size(ptr);
}



void *operator new(size_t size) { return halloc(size); }
void *operator new[](size_t size) { return halloc(size); }
void operator delete(void *ptr) { hfree(ptr); }
void operator delete[](void *ptr) { hfree(ptr); }


extern "C" {

// void *test_halloc(size_t size) { return halloc(size); }
// void test_hfree(void *ptr) { hfree(ptr); }

void *malloc(size_t size) { return halloc(size); }
void *calloc(size_t size, size_t count) { return hcalloc(size, count); }
void *realloc(void *ptr, size_t newsize) { return hrealloc(ptr, newsize); }
void free(void *ptr) { hfree(ptr); }
size_t malloc_usable_size(void *ptr) { return halloc_usable_size(ptr); }
}
