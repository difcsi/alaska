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

#include <ck/map.h>
#include <alaska/alaska.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/ThreadCache.hpp>
#include <stdint.h>

ck::HashTable<void*> nullcount_map;

// Defined in halloc.cpp -- the calling thread's (raw) thread cache.
extern alaska::ThreadCache *get_tc_r(void);


extern "C" {

/*
  HACK: this is not the most sophisticated way to prevent recursion, but it works for now... 
  
  For some reason, the refcount operations themselves can trigger refcount operations (e.g. if we need to read/write a handle's metadata).
  This shouldn't really happen as alaska functions are supposed to be escaped
*/
static thread_local bool in_refcount_operation = false;

/**
 * alaska_inc_refcount - Increment the reference count of a handle
 * 
 * This function is called by the compiler when a handle is written to heap memory.
 * It checks if the pointer is actually a handle and increments its refcount.
 * 
 * @param ptr - The potential handle whose refcount should be incremented
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_inc_refcount(void *ptr) {
  // print what is at the ptr in hex
  
  if (ptr == nullptr) return; 
  if (in_refcount_operation) return;
  
  in_refcount_operation = true;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do
    in_refcount_operation = false;
    return;
  }

  // alaska::printf("Incrementing refcount of mapping  %p from %lu\n", mapping, mapping->get_refcount());
  // Increment the refcount using the mapping's method
  auto new_count = mapping->inc_refcount();
  if(new_count == 1) {
    nullcount_map.remove(ptr);
  }
  
  in_refcount_operation = false;
}

/**
 * alaska_dec_refcount - Decrement the reference count of a handle
 * 
 * This function is called by the compiler when a handle is being overwritten.
 * It checks if the pointer is actually a handle and decrements its refcount.
 * If the refcount reaches zero, the handle could potentially be freed.
 * 
 * @param ptr - The potential handle whose refcount should be decremented
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_dec_refcount(void *ptr) {
  if (ptr == nullptr) return;
  
  // Prevent infinite recursion if this function itself triggers refcount operations
  if (in_refcount_operation) return;
  in_refcount_operation = true;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do
    in_refcount_operation = false;
    return;
  }

  // Decrement the refcount using the mapping's method
  uint64_t new_count = mapping->dec_refcount();
  
  // TODO: If refcount reaches 0, we could potentially free the handle
  // For now, we just track the refcount. The actual freeing policy
  // should attach here
  if (new_count == 0) {
   nullcount_map.set(ptr);
  } else {
   // The refcount dropped but is still non-zero. This is the only situation in
   // which `mapping` can become the root of a garbage *cycle*, so hand it to
   // Anchorage's cycle collector as a candidate root (Bacon & Rajan "purple").
   alaska::Runtime::get().cycle_collector.register_candidate(mapping);
  }

  in_refcount_operation = false;
}

/**
 * alaska_get_refcount - Get the current reference count of a handle
 * 
 * This function allows users to query the reference count of a handle.
 * It checks if the pointer is actually a handle and returns its refcount.
 * 
 * @param ptr - The potential handle whose refcount should be retrieved
 * @return The refcount if ptr is a valid handle, or 0 if ptr is NULL or not a handle
 */
unsigned long alaska_get_refcount(void *ptr) {
  if (ptr == nullptr) return 0;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, return 0
    return 0;
  }
  
  // Get and return the refcount using the mapping's method
  return (unsigned long)mapping->get_refcount();
}

int alaska_nullcount_map_size(){
  return nullcount_map.size();
}

void alaska_nullcount_map_foreach(void (*fn)(void* ptr)) {
  for (auto it = nullcount_map.begin(); it != nullcount_map.end(); ++it) {
    fn(*it);
  }
}

inline int alaska_is_handle(void *ptr){
  return alaska::Mapping::is_handle(ptr);
}

/**
 * alaska_collect_cycles - Run one cycle collection inside Anchorage.
 *
 * Stops the world (via Anchorage's barrier) and runs synchronous trial-deletion
 * cycle collection over the buffered candidate roots, reclaiming any handles
 * that are only kept alive by reference cycles. Returns the number of handles
 * reclaimed. Note the barrier has a minimum interval, so back-to-back calls may
 * return 0 simply because no barrier was taken.
 */
unsigned long alaska_collect_cycles(void) {
  auto &rt = alaska::Runtime::get();
  // Make sure this thread has a thread cache *before* entering the barrier:
  // creating one needs locks the barrier already holds.
  auto *tc = get_tc_r();
  unsigned long reclaimed = 0;
  rt.with_barrier([&]() { reclaimed = rt.cycle_collector.collect(*tc); });
  return reclaimed;
}

// Number of candidate cycle roots currently buffered.
unsigned long alaska_cycle_candidate_count(void) {
  return alaska::Runtime::get().cycle_collector.candidate_count();
}

// Total number of handles reclaimed by the cycle collector so far.
unsigned long alaska_cycles_collected(void) {
  return alaska::Runtime::get().cycle_collector.total_collected();
}
}  // extern "C"
