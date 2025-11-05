/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */


// This file contains the initialization and deinitialization functions for the
// alaska::Runtime instance, as well as some other bookkeeping logic.

#include <alaska/Runtime.hpp>
#include <alaska/alaska.hpp>
#include <rt/barrier.hpp>
#include "alaska/SizeClass.hpp"
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <ck/queue.h>
#include <alaska/RateCounter.hpp>

static alaska::Runtime *the_runtime = nullptr;


struct CompilerRuntimeBarrierManager : public alaska::BarrierManager {
  ~CompilerRuntimeBarrierManager() override = default;
  bool begin(void) override { return alaska::barrier::begin(); }
  void end(void) override { alaska::barrier::end(); }
};

static CompilerRuntimeBarrierManager the_barrier_manager;

extern "C" void alaska_dump(void) { the_runtime->dump(stderr); }


static pthread_t barrier_thread;
static void *barrier_thread_func(void *) {
  bool in_marking_state = true;



  while (1) {
    auto &rt = alaska::Runtime::get();
    usleep(75 * 1000);
    continue;

    long already_invalid = 0;
    long total_handles = 0;
    long newly_marked = 0;

    float cold_perc = 0;
    // Linear Congruential Generator parameters
    unsigned int seed = 123456789;  // You can set this to any initial value


    int stride = 20;
    int offset = 0;

    rt.with_barrier([&]() {
      // printf("\033[2J\033[H");
      // // rt.handle_table.dump(stdout);
      rt.heap.dump(stdout);
      rt.heap.compact_sizedpages();
      return;
    });
  }

  return NULL;
}

void __attribute__((constructor(102))) alaska_init(void) {
  // Allocate the runtime simply by creating a new instance of it. Everywhere
  // we use it, we will use alaska::Runtime::get() to get the singleton instance.
  the_runtime = new alaska::Runtime();
  // Attach the runtime's barrier manager
  the_runtime->barrier_manager = &the_barrier_manager;
  pthread_create(&barrier_thread, NULL, barrier_thread_func, NULL);
}

void __attribute__((destructor)) alaska_deinit(void) {}
