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
#include <assert.h>
#include <sys/signal.h>


#include "./shared.h"



extern "C" void yukon_enable_localization(int enable) {}

// -------------------------------------------------------------- //
//                     Allocation Interface                       //
// -------------------------------------------------------------- //



static void *_halloc(size_t sz, int zero) {
  void *result = NULL;

  alaska::LockedThreadCache tc = *yukon_get_tc();
  result = tc->malloc(sz);
  if (zero && result) memset(result, 0, sz);
  return result;
}

extern "C" void *halloc(size_t sz) noexcept {
  INSTRUCTION_TRACKER(INSTCOUNT_MALLOC);
  LocalizationLatch loc_latch;
  return _halloc(sz, 0);
}
extern "C" void *hcalloc(size_t nmemb, size_t size) {
  INSTRUCTION_TRACKER(INSTCOUNT_CALLOC);
  LocalizationLatch loc_latch;
  return _halloc(nmemb * size, 1);
}

// Reallocate a handle
extern "C" void *hrealloc(void *ptr, size_t new_size) {
  // If the ptr is null, then this call is equivalent to malloc(size)
  if (ptr == NULL) {
    return halloc(new_size);
  }

  // If the size is equal to zero, and the ptr is not null, realloc acts like free(ptr)
  if (new_size == 0) {
    // If it wasn't a ptr, just forward to the system realloc
    hfree(ptr);
    return NULL;
  }

  INSTRUCTION_TRACKER(INSTCOUNT_REALLOC);
  LocalizationLatch loc_latch;
  alaska::LockedThreadCache tc = *yukon_get_tc();
  return tc->realloc(ptr, new_size);
}



extern "C" void hfree(void *ptr) {
  INSTRUCTION_TRACKER(INSTCOUNT_FREE);
  LocalizationLatch loc_latch;
  // AutoFencer fencer;
  // no-op if NULL is passed
  if (unlikely(ptr == NULL)) return;
  alaska::LockedThreadCache tc = *yukon_get_tc_unchecked();
  tc->free(ptr);
}


extern "C" size_t halloc_usable_size(void *ptr) {
  INSTRUCTION_TRACKER(INSTCOUNT_GETSIZE);
  auto tc = yukon_get_tc_unchecked();
  return tc->get_size(ptr);
}


// -------------------------------------------------------------- //
//                        Libc Overrides                          //
// -------------------------------------------------------------- //


void *operator new(size_t size) { return halloc(size); }
void *operator new[](size_t size) { return halloc(size); }
void operator delete(void *ptr) { hfree(ptr); }
void operator delete(void *ptr, unsigned long) { hfree(ptr); }
void operator delete[](void *ptr) { hfree(ptr); }


extern "C" {
void *malloc(size_t size) { return halloc(size); }
void *calloc(size_t size, size_t count) { return hcalloc(size, count); }
void *realloc(void *ptr, size_t newsize) { return hrealloc(ptr, newsize); }
void free(void *ptr) { hfree(ptr); }
size_t malloc_usable_size(void *ptr) { return halloc_usable_size(ptr); }
}
