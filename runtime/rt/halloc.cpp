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

// #define MALLOC_BYPASS

#ifdef MALLOC_BYPASS
#include <malloc.h>
#endif

#include <ck/stack.h>
#include <ck/queue.h>
#include <ck/vec.h>
#include <ck/set.h>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <alaska.h>
#include <errno.h>



// TODO: don't have this be global!
static __thread alaska::ThreadCache *g_tc = nullptr;



alaska::ThreadCache *get_tc_r(void) {
  if (unlikely(g_tc == nullptr)) {
    g_tc = alaska::Runtime::get().new_threadcache();
  }
  return g_tc;
}
alaska::LockedThreadCache get_tc(void) { return *get_tc_r(); }




static void *_halloc(size_t sz, int zero) {
  void *result = get_tc()->halloc(sz);

  // This seems right...
  if (result == NULL) errno = ENOMEM;



  if (zero) {
    // handle translate.
    alaska::handle_memset(result, 0, sz);
  }
  return result;
}


void *halloc(size_t sz) noexcept {
#ifdef MALLOC_BYPASS
  return ::malloc(sz);
#endif
  return _halloc(sz, 0);
}

void *hcalloc(size_t nmemb, size_t size) {
#ifdef MALLOC_BYPASS
  return ::calloc(nmemb, size);
#endif
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
void *hrealloc(void *handle, size_t new_size) {
#ifdef MALLOC_BYPASS
  return ::realloc(handle, new_size);
#endif
  // If the handle is null, then this call is equivalent to malloc(size)
  if (handle == NULL) return halloc(new_size);


  auto *m = alaska::Mapping::from_handle_safe(handle);
  // If the size is equal to zero, and the handle is not null, realloc acts like free(handle)
  if (new_size == 0) {
    log_debug("realloc edge case: zero size %p!", handle);
    // If it wasn't a handle, just forward to the system realloc
    hfree(handle);
    return NULL;
  }

  handle = get_tc()->hrealloc(handle, new_size);
  return handle;
}



void hfree(void *ptr) {
#ifdef MALLOC_BYPASS
  return ::free(ptr);
#endif
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;

#ifdef ALASKA_HTLB_SIM
  extern void alaska_htlb_sim_invalidate(uintptr_t handle);
  alaska_htlb_sim_invalidate((uintptr_t)ptr);
#endif

  // Simply ask the thread cache to free it!
  get_tc()->hfree(ptr);
}




size_t alaska_usable_size(void *ptr) {
#ifdef MALLOC_BYPASS
  return ::malloc_usable_size(ptr);
#endif
  return get_tc()->get_size(ptr);
}




// template <typename Fn>
// static void walk_structure(void *ptr, size_t max_depth, Fn fn) {
//   auto &rt = alaska::Runtime::get();
//   auto *tc = get_tc_r();
//   ck::queue<void *> todo(max_depth);

//   auto schedule_pointer = [&](void *h, alaska::Mapping *m) {
//     if (m == NULL or not rt.handle_table.valid_handle(m) or m->is_free()) return;
//     fn(m);
//     if (todo.size() >= max_depth) return;
//     todo.push(h);
//   };

//   auto *m = alaska::Mapping::from_handle_safe(ptr);
//   // Fire off a check of the first pointer to bootstrap the localization
//   schedule_pointer(ptr, m);

//   while (not todo.is_empty()) {
//     auto *h = todo.pop().unwrap();
//     auto *m = alaska::Mapping::from_handle_safe(h);
//     if (m == nullptr) continue;
//     long size = tc->get_size(h);
//     long elements = size / 8;

//     void **cursor = (void **)m->get_pointer();
//     for (long e = 0; e < elements; e++) {
//       void *c = cursor[e];
//       auto *m = alaska::Mapping::from_handle_safe(c);
//       if (m) {
//         schedule_pointer(c, m);
//       }
//     }
//   }
// }



static long seen = 0;

static long localize_structure_impl(alaska::Mapping *m, int depth, alaska::ThreadCache &tc) {
  long localized = 0;
  seen++;


  auto header = alaska::ObjectHeader::from(m);
  uint64_t *start = (uint64_t *)m->get_pointer();
  uint64_t *end = (uint64_t *)((char *)start + header->object_size());
  /*
  alaska::printf("%6ld ", seen);
  for (int i = 0; i < depth - 1; i++) alaska::printf("|  ");
  alaska::printf("|--");
  alaska::printf("%p,%p %zu | ", m, m->get_pointer(), header->object_size());
  // gray
  alaska::printf("\e[90m");
  for (uint64_t *p = start; p < end; p++) {
    alaska::printf("%016lx ", *p);
  }
  alaska::printf("\e[0m\n");
  */

  // if (depth > 2000) return localized;

  // then, walk its data

  if (!header->localized) {
    localized += (long)tc.localize(m, 0);
  }

  if (depth == 0) return localized;

  header = alaska::ObjectHeader::from(m);

  // header->walk([&](alaska::Mapping *om, alaska::ObjectHeader *oheader) {
  //   if (oheader->localized) return;
  //   localized += (long)tc.localize(om, 0);
  // });

  header->walk([&](alaska::Mapping *om, alaska::ObjectHeader *oheader) {
    localized += localize_structure_impl(om, depth - 1, tc);
  });
  return localized;
}

extern "C" bool localize_structure(void *ptr) {
  auto &rt = alaska::Runtime::get();

  // auto *tc = get_tc_r();
  // rt.with_barrier([&]() {
  //   rt.grade_heap();
  //   long localized = 0;
  //   rt.handle_table.for_each_handle([&](alaska::Mapping *m) {
  //     localized += localize_structure_impl(m, 2000, *tc);
  //   });
  //   alaska::printf("Localized %ld objects\n", localized);
  //   rt.grade_heap();
  // });

  rt.with_barrier([&]() {
    auto *tc = get_tc_r();
    auto *m = alaska::Mapping::from_handle(ptr);
    seen = 0;

    long loc_depth = 200;
    auto report_before = alaska::grade_locality(*m, 16);
    rt.grade_heap();

    auto localize_count = localize_structure_impl(m, loc_depth, *tc);
    alaska::printf("Localized %ld objects, seen = %lu\n", localize_count, seen);
    auto report_after = alaska::grade_locality(*m, 16);

    rt.grade_heap();

    alaska::printf("Locality report before:\n");
    report_before.dump();
    alaska::printf("Locality report after:\n");
    report_after.dump();
  });

  return true;
}
