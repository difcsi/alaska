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

#include <alaska/alaska.hpp>
#include <stdint.h>

extern "C" {

// Thread-local guard to prevent recursion
static thread_local bool in_refcount_operation = false; //  HACK: this is not the most sophisticated way to prevent recursion, but it works for now

/**
 * alaska_inc_refcount - Increment the reference count of a handle
 * 
 * This function is called by the compiler when a handle is written to memory.
 * It checks if the pointer is actually a handle and increments its refcount.
 * 
 * @param ptr - The potential handle whose refcount should be incremented
 */
__attribute__((section("$__ALASKA__refcount")))
void alaska_inc_refcount(void *ptr) {
  // print what is at the ptr in hex
  
  if (ptr == nullptr) return; 
  
  in_refcount_operation = true;
  
  // Check if this is actually a handle
  auto mapping = alaska::Mapping::from_handle_safe(ptr);
  if (mapping == nullptr) {
    // Not a handle, nothing to do
    return;
  }

  // alaska::printf("Incrementing refcount of mapping  %p from %lu\n", mapping, mapping->get_refcount());
  // Increment the refcount using the mapping's method
  mapping->inc_refcount();
  
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
    return;
  }

  // Decrement the refcount using the mapping's method
  uint64_t new_count = mapping->dec_refcount();
  
  // TODO: If refcount reaches 0, we could potentially free the handle
  // For now, we just track the refcount. The actual freeing policy
  // should attach here
  if (new_count == 0) {
    // TODO: unset remove lifetime policy bit flag
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

}  // extern "C"
