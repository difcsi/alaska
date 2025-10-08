
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


#include <alaska/Runtime.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/BarrierManager.hpp>
#include <alaska/Localizer.hpp>
#include "alaska/alaska.hpp"
#include "alaska/utils.h"
#include <stdlib.h>
#include <alaska/utils.h>
#include <alaska/alaska_internal_malloc.h>

namespace alaska {
  // The default instance of a barrier manager.
  static BarrierManager global_nop_barrier_manager;
  // The current global instance of the runtime, since we can only have one at a time
  static Runtime *g_runtime = nullptr;
  static volatile bool runtime_initialized = false;


  Runtime::Runtime(alaska::Configuration config)
      : config(config)
      , handle_table(config)
      , heap(config) {
    if (g_runtime != nullptr) {
      log_error("Cannot create a new Alaska Runtime, one already exists at %p", g_runtime);
      abort();
    }


    // Assign the global runtime to be this instance
    atomic_set(g_runtime, this);
    // Attach a default barrier manager
    this->barrier_manager = &global_nop_barrier_manager;

    log_debug("Created a new Alaska Runtime @ %p", this);
    atomic_set(runtime_initialized, true);
  }

  Runtime::~Runtime() {
    log_debug("Destroying Alaska Runtime");
    // Unset the global instance so anoruntime can be allocated
    atomic_set(g_runtime, nullptr);
  }


  Runtime &Runtime::get() {
    ALASKA_ASSERT(g_runtime != nullptr, "Runtime not initialized");
    return *g_runtime;
  }
  Runtime *Runtime::get_ptr() { return g_runtime; }

  bool Runtime::is_valid_handle(void *p) {
    alaska::Mapping *m = alaska::Mapping::from_handle_safe(p);
    if (m == nullptr) return false;
    return this->handle_table.valid_handle(m);
  }


  void Runtime::dump(FILE *stream) {
#define xstr(s) str(s)
#define str(s) #s
    fprintf(stream, "Revision: " xstr(ALASKA_REVISION) "\n");
#undef xstr
#undef str

    fprintf(stream, "barriers: %zu (%.1f/s)\n", stat_barriers.read(), stat_barriers.digest());

    for (auto *tc : this->tcs) {
      fprintf(stream, "tc%d allocations:%zu (%.1f/s)", tc->id, tc->allocation_rate.read(),
          tc->allocation_rate.digest());
      fprintf(stream, "  frees:%zu (%.1f/s)", tc->free_rate.read(), tc->free_rate.digest());
      fprintf(stream, "  heap_churn:%zu (%.1f/s)", tc->heap_churn.read(), tc->heap_churn.digest());
      fprintf(stream, "  ht_churn:%zu (%.1f/s)", tc->handle_table_churn.read(),
          tc->handle_table_churn.digest());
      fprintf(stream, "\n");
    }
    handle_table.dump(stream);
    heap.dump(stream);
  }


  void Runtime::dump_html(FILE *stream) {
    fprintf(stream, "<!DOCTYPE html>\n");
    fprintf(stream, "<html>\n");

    fprintf(stream, R"HEAD(
       <head>
         <style>
            :root { --p0-color: #ff00ff; --p1-color: #9C27B0; }
            body { box-sizing: border-box; color: white; background-color: black; text-wrap: nowrap; font-family: monospace; }
            /* td { text-wrap: nowrap; } */
            /* td { line-height: 0; } */
            .el { height: 15px; background-color: #2f2f2f; display: inline-block; /* border-right: 1px solid black; */ }
            .al { background-color: #2fff2f; color: black; }
            .fr { background-color: black !important; }
            .localitydata .al { background-color: white; }
            .p0 { border-bottom: 2px solid var(--p0-color); border-top: 2px solid var(--p0-color); }
            .p1 { border-bottom: 2px solid var(--p1-color); border-top: 2px solid var(--p1-color); }
            .localitydata .p0 { background-color: var(--p0-color); }
            .localitydata .p1 { background-color: var(--p1-color); }
            .pin { background-color: red !important; }


         </style>
       </head>
    )HEAD");
    fprintf(stream, "<body>\n");

    fprintf(stream, "<h1>Heap Pages:</h1>");

    fprintf(stream, "<table>");
    heap.dump_html(stream);
    fprintf(stream, "</table>");


    fprintf(stream, "</body><html>\n");
  }


  ThreadCache *Runtime::new_threadcache(void) {
    auto tc = new ThreadCache(next_thread_cache_id++, *this);
    tcs_lock.lock();
    tcs.add(tc);
    tcs_lock.unlock();
    return tc;
  }

  void Runtime::del_threadcache(ThreadCache *tc) {
    tcs_lock.lock();
    tcs.remove(tc);
    delete tc;
    tcs_lock.unlock();
  }


  void Runtime::lock_all_thread_caches(void) {
    tcs_lock.lock();

    for (auto *tc : tcs)
      tc->lock.lock();
  }
  void Runtime::unlock_all_thread_caches(void) {
    for (auto *tc : tcs)
      tc->lock.unlock();
    tcs_lock.unlock();
  }


  void wait_for_initialization(void) {
    log_debug("waiting for initialization!\n");
    while (not is_initialized()) {
      sched_yield();
    }
    log_debug("Initialized!\n");
  }


  int do_handle_fault(uint64_t handle) {
    auto &rt = alaska::Runtime::get();
    return rt.handle_fault(handle);
  }


  int Runtime::handle_fault(uint64_t handle) {
    auto *m = alaska::Mapping::from_handle((void *)handle);
    // printf("fault on %p\n", m);
    handle_faults.track_atomic(1);
    return 0;
  }


  bool is_initialized(void) { return atomic_get(runtime_initialized); }


  Runtime::HeapReport Runtime::grade_heap(void) {
    auto start_time = alaska_timestamp();
    HeapReport report{0};



    struct SizeclassStats {
      size_t count = 0;
      uint64_t in_pointers = 0;
      uint64_t out_pointers = 0;
    };

    SizeclassStats size_class_histogram[alaska::num_size_classes] = {};

    handle_table.for_each_handle([&](alaska::Mapping *m) {
      void *p = m->get_pointer();
      if (p == nullptr) return;
      auto header = alaska::ObjectHeader::from(p);
      if (!header) return;

      size_t obj_size = header->object_size();

      size_t sc = alaska::size_to_class(obj_size);
      if (sc >= alaska::num_size_classes) {
        return;  // continue, ignore invalid size classes
      }
      size_class_histogram[sc].count++;

      report.total_handles++;
      report.object_bytes += obj_size;

      uintptr_t object_page = (uintptr_t)p >> 12;
      uintptr_t handle_page = (uintptr_t)m >> 12;

      // alaska::printf("Walking object %p (size=%zu, data=%p)\n", m, obj_size, header->data());
      // Walk the mapping to count in/out pointers.
      header->walk([&](alaska::Mapping *om, alaska::ObjectHeader *oheader) {
        void *p = oheader->data();
        uintptr_t opage = (uintptr_t)p >> 12;
        if (opage != object_page) {
          size_class_histogram[sc].out_pointers++;
          report.out_pointers++;
        } else {
          size_class_histogram[sc].in_pointers++;
          report.in_pointers++;
        }

        uintptr_t hpage = (uintptr_t)om >> 12;
        if (hpage != handle_page) {
          report.out_handles++;
        } else {
          report.in_handles++;
        }
      });
    });


    // Walk over every heap page to count committed bytes.
    heap.for_each_page([&](alaska::HeapPage *page) {
      report.committed_bytes += page->committed_bytes();
    });


    auto end_time = alaska_timestamp();

    // TODO: remove this dumping when done debugging.
    alaska::printf("-- HEAP GRADE REPORT --\n");
    alaska::printf("Graded heap in %.3f ms\n", (end_time - start_time) / 1e6f);
    alaska::printf("Total committed bytes:      %zu\n", report.committed_bytes);
    alaska::printf("Total handles:              %zu\n", report.total_handles);
    alaska::printf(
        "Handle table bytes:         %zu\n", report.total_handles * sizeof(alaska::Mapping));
    alaska::printf("Total object bytes:         %zu\n", report.object_bytes);
    alaska::printf("Average Size:               %.1f\n",
        report.total_handles == 0 ? 0.0
                                  : (double)report.object_bytes / (double)report.total_handles);
    alaska::printf("Heap Utilization:           %.2f%%\n",
        report.committed_bytes == 0
            ? 0.0
            : 100.0 * (double)report.object_bytes / (double)report.committed_bytes);

    // in/out pointers
    if (report.in_pointers + report.out_pointers > 0) {
      alaska::printf("Pointer Locality:           %.2f%%\n",
          100.0 * (double)report.in_pointers / (double)(report.in_pointers + report.out_pointers));
    } else {
      alaska::printf("Pointer Locality:           N/A\n");
    }
    alaska::printf("    In-Pointers:            %zu\n", report.in_pointers);
    alaska::printf("    Out-Pointers:           %zu\n", report.out_pointers);


    // in/out handles
    if (report.in_handles + report.out_handles > 0) {
      alaska::printf("Handle Locality:            %.2f%%\n",
          100.0 * (double)report.in_handles / (double)(report.in_handles + report.out_handles));
    } else {
      alaska::printf("Handle Locality:            N/A\n");
    }
    alaska::printf("    In-Handles:             %zu\n", report.in_handles);
    alaska::printf("    Out-Handles:            %zu\n", report.out_handles);

    alaska::printf("\nSize Class Histogram:\n");
    alaska::printf(" Size Class | Size |  Count  |  In Ptrs  | Out Ptrs |  Locality \n");
    alaska::printf("-------------------------------------------------------------\n");
    for (size_t i = 0; i < alaska::num_size_classes; i++) {
      size_t count = size_class_histogram[i].count;
      if (count == 0) continue;
      uint64_t in_ptrs = size_class_histogram[i].in_pointers;
      uint64_t out_ptrs = size_class_histogram[i].out_pointers;
      double locality =
          in_ptrs + out_ptrs == 0 ? 0.0 : 100.0 * (double)in_ptrs / (double)(in_ptrs + out_ptrs);
      alaska::printf(" %10zu | %4zu | %7zu | %9zu | %8zu | %8.2f%% \n", i, alaska::class_to_size(i),
          count, in_ptrs, out_ptrs, locality);
    }




    alaska::printf("-----------------------\n");



    return report;
  }

}  // namespace alaska


// Simply use clock_gettime, which is fast enough on most systems
extern "C" uint64_t alaska_timestamp() {
  struct timespec spec;
  clock_gettime(1, &spec);
  return spec.tv_sec * (1000 * 1000 * 1000) + spec.tv_nsec;
}



static void __attribute__((destructor)) alaska_runtime_deinit(void) {
  if (alaska::g_runtime) {
    if (getenv("ALASKA_INFO") == NULL) return;

    auto &rt = alaska::Runtime::get();
    rt.dump(stdout);
  }
}