/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * liballocs integration: raw-pointer metadata helpers.
 *
 * These functions let liballocs answer metadata queries (base/size) for objects
 * living in Alaska's backing heap. They operate purely on *raw backing pointers*
 * -- the kind liballocs already holds after stackscan's translate-then-index --
 * and never inspect the handle bit. Handles are an Alaska concept; liballocs
 * stays handle-agnostic and only learns about a heap region of raw addresses.
 *
 * The symbols are exported with default visibility so liballocs (loaded as a
 * separate DSO / preload) can weak-import them at load time, mirroring how
 * stackscan's handle_query.c weak-imports alaska_translate.
 */

#include <alaska/Runtime.hpp>
#include <alaska/Heap.hpp>
#include <alaska/HeapPage.hpp>
#include <stddef.h>

#define ALASKA_EXPORT __attribute__((visibility("default")))

// Start of Alaska's contiguous backing heap, or NULL before the runtime exists.
extern "C" ALASKA_EXPORT void *alaska_heap_start(void) {
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  return rt->heap.pm.get_start();
}

// Size in bytes of Alaska's contiguous backing heap.
extern "C" ALASKA_EXPORT unsigned long alaska_heap_size(void) {
  return (unsigned long)alaska::heap_size;
}

// Given a raw BACKING pointer (interior pointers allowed), return the start of
// the object that contains it, or NULL if the pointer is not in Alaska's sized
// heap (e.g. huge objects live in separate mmaps and are not covered here).
extern "C" ALASKA_EXPORT void *alaska_object_base(void *backing) {
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  alaska::HeapPage *page = rt->heap.pt.get_unaligned(backing);
  if (page == nullptr) return nullptr;
  return page->object_base(backing);
}

// Size in bytes of the object containing a raw BACKING pointer, or 0 if not in
// Alaska's sized heap. NOTE: this works on the backing pointer directly via the
// page table -- unlike alaska_usable_size(), which expects a *handle* and routes
// a raw pointer to the huge allocator (returning 0 for sized-heap objects).
extern "C" ALASKA_EXPORT unsigned long alaska_object_size(void *backing) {
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  alaska::HeapPage *page = rt->heap.pt.get_unaligned(backing);
  if (page == nullptr) return 0;
  return (unsigned long)page->size_of(backing);
}

// For a raw BACKING pointer that lands in an *allocated* Alaska heap page,
// report the extent [base, base+size) of the containing page and return 1;
// return 0 otherwise. liballocs uses this to lazily claim that page as a
// bigalloc the first time a query for it misses, so queries route to the
// Alaska allocator. Page-granular (not whole-heap) so each bigalloc stays well
// under liballocs' max-bigalloc size and nesting stays simple.
extern "C" ALASKA_EXPORT int alaska_heap_page_extent(
    void *backing, void **out_base, unsigned long *out_size) {
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  void *start = rt->heap.pm.get_start();
  if (backing < start) return 0;
  uintptr_t off = (uintptr_t)backing - (uintptr_t)start;
  if (off >= alaska::heap_size) return 0;
  // Only claim pages that actually back a live sized object.
  if (rt->heap.pt.get_unaligned(backing) == nullptr) return 0;
  uintptr_t page_ind = off / alaska::page_size;
  if (out_base) *out_base = (void *)((uintptr_t)start + page_ind * alaska::page_size);
  if (out_size) *out_size = (unsigned long)alaska::page_size;
  return 1;
}
