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
#include "alaska.h"
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



// static inline uint64_t read_csr() {
//   uint64_t cycles;
//   asm volatile("rdcycle %0" : "=r"(cycles));
//   return cycles;
// }

static inline uint64_t read_cycle_counter() {
  uint64_t cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
}


static uint64_t instructions_in_runtime = 0;




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

    exit(-11);
    // write_csr(CSR_HTINVAL, ((1LU << (64 - ALASKA_SIZE_BITS)) - 1));
    return;
  }


  // Pause requested.
  if (sig == SIGUSR2) {
    return;
  }
}




static volatile bool delayed_localization_pending = false;
static volatile long localization_latch_depth = 0;
static long localizations = 0;
static alaska::RateCounter immediate_localizations;
static alaska::RateCounter delayed_localizations;

static void schedule_localization(uint64_t interval_override = 0) {
  uint64_t interval = interval_override != 0 ? interval_override : yukon_dump_interval;
  // schedule the next dump in the future!
  if ((long)ualarm(interval, 0) == -1) {
    perror("Failed to setup ualarm for dumping");
    exit(-1);
  }
}

static bool attempt_localization(void) {
  // auto start = alaska_timestamp();
  if (localization_latch_depth > 0) {
    return false;
  }
  // Grab the thread cache
  auto *tc = yukon::get_dump_tc();
  // And trigger an HTLB dump
  yukon::dump_htlb(tc);
  localizations++;
  // auto end = alaska_timestamp();
  // alaska::printf("[localized] time: %8.2fus\n", (end - start) / 1000.0);

  return true;
}

void alarm_handler(int sig) {
  if (!alaska::is_initialized()) {
    schedule_localization();
    return;
  }
  if (attempt_localization()) {
    // if localization succeeded, track it
    immediate_localizations.track();
    // Then, schedule the next one
    schedule_localization();
  } else {
    delayed_localization_pending = true;
  }
}

struct LocalizationLatch {
  LocalizationLatch() { localization_latch_depth++; }

  ~LocalizationLatch() {
    localization_latch_depth--;
    if (delayed_localization_pending) {
      if (attempt_localization()) {
        // if it didn't succeed, we scheduled a delayed one
        // and should track that.
        delayed_localizations.track();
        delayed_localization_pending = false;
        schedule_localization();
      }
    }
  }
};

struct AutoFencer {
  uint64_t start_inst;
  AutoFencer() { start_inst = read_instret(); }
  ~AutoFencer() {
    __asm__ volatile("fence" ::: "memory");
    instructions_in_runtime += (read_instret() - start_inst);
  }
};


pthread_t dump_thread;

static void setup_signal_handlers(void) {
  unsetenv("LD_PRELOADLOAD");
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
  assert(sigaction(SIGSEGV, &sa, NULL) == 0);
  // assert(sigaction(SIGUSR2, &sa, NULL) == 0);
}



#define ALASKA_THREAD_TRACK_STATE_T int
#define ALASKA_THREAD_TRACK_INIT setup_signal_handlers();
#include <alaska/thread_tracking.in.hpp>


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
    if (alaska_fd == -1) alaska_fd = open("/dev/alaska", O_RDONLY);
    static size_t localization_count = 0;

    auto &rt = alaska::Runtime::get();

    rt.with_barrier([&]() {
      // The size of the HTLB
      size_t size = 16 + 512;
      auto *space = tc->localizer.get_hotness_buffer(size);
      memset(space, 0, size * sizeof(alaska::handle_id_t));
      int r = read(alaska_fd, space, size * sizeof(alaska::handle_id_t));
      tc->localizer.feed_hotness_buffer(size, space);


      auto &ht = rt.handle_table;
      if (localization_count++ % 50 == 0) {
        int hot_cutoff = 8;
        size_t hot_bytes = 0;
        size_t cold_bytes = 0;

        size_t total_heap_sw_pages = rt.heap.pm.get_allocated_page_count();
        size_t total_heap_hw_pages = total_heap_sw_pages * (alaska::page_size / 4096);


        size_t hot_pages = 0;
        uint8_t hot_page_bitmap[total_heap_hw_pages / 8];
        memset(hot_page_bitmap, 0, total_heap_hw_pages / 8);

        auto track_hot_page = [&](uintptr_t page) {
          // page is an absolute page. we need to offset it by the start of the heap
          uintptr_t heap_start_page = (uintptr_t)rt.heap.pm.get_start() >> 12;
          page -= heap_start_page;  // here we are hoping the pointer is valid!


          // if the page is already set, do nothing. otherwise, incrememt hot_pages and set the bit
          if (hot_page_bitmap[page / 8] & (1 << (page % 8))) return;
          hot_pages++;
          hot_page_bitmap[page / 8] |= (1 << (page % 8));
        };

        auto slabs = ht.get_slabs();
        for (auto *slab : slabs) {
          for (auto *allocated : slab->allocator) {
            auto *m = (alaska::Mapping *)allocated;
            if (m->get_pointer() == nullptr) continue;
            uintptr_t page = (uintptr_t)m->get_pointer() >> 12;
            auto header = alaska::ObjectHeader::from(m->get_pointer());
            if (header->hotness > hot_cutoff) {
              track_hot_page(page);
              hot_bytes += header->object_size();
            }
          }
        }

        auto total_bytes = hot_bytes + cold_bytes;
        auto pages_needed_for_hot_objects = round_up(hot_bytes, 4096) / 4096;
        float hot_utilization = pages_needed_for_hot_objects / (float)hot_pages;
        size_t cold_pages = total_heap_hw_pages - hot_pages;

        alaska::printf(
            "[dump %5zu] hot/cold pages: %5zu(%4zu needed %5f)/%5zu | hot/cold bytes: "
            "%12zu/%12zu\n",
            localization_count, hot_pages, pages_needed_for_hot_objects, hot_utilization,
            cold_pages, hot_bytes, cold_bytes);
      }
    });
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


    if (getenv("NODUMP") == nullptr) {
      signal(SIGALRM, alarm_handler);

      yukon_dump_interval = 5 * 1000;
      // Schedule the first dump for 50ms from now.
      schedule_localization(50 * 1000);
    }
  }
}  // namespace yukon



void __attribute__((constructor(102))) alaska_init(void) {
  unsetenv("LD_PRELOAD");  // make it so we don't run alaska in subprocesses!
  // allocate
  yukon::get_tc();
}

void __attribute__((destructor)) alaska_deinit(void) {
  alaska::printf("YUKON_RTINST=%zu\n", instructions_in_runtime);
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
  // if (m) {
  //   auto backing_data = (uintptr_t)m->get_pointer();
  //   touch_pages(backing_data, backing_data + sz);
  // }
  if (result == NULL) errno = ENOMEM;

  return result;
}

extern "C" void *halloc(size_t sz) noexcept {
  LocalizationLatch loc_latch;
  AutoFencer fencer;
  return _halloc(sz, 0);
}
extern "C" void *hcalloc(size_t nmemb, size_t size) {
  LocalizationLatch loc_latch;
  AutoFencer fencer;
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
extern "C" void *hrealloc(void *handle, size_t new_size) {
  LocalizationLatch loc_latch;
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
  LocalizationLatch loc_latch;
  AutoFencer fencer;
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;


  alaska::LockedThreadCache tc = *yukon::get_tc();
  tc->hfree(ptr);
}


extern "C" size_t halloc_usable_size(void *ptr) {
  LocalizationLatch loc_latch;
  alaska::LockedThreadCache tc = *yukon::get_tc();
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
