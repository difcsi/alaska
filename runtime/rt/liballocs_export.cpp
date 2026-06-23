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
  // The Runtime singleton pointer (g_runtime) is published before the heap/page
  // table are constructed, so get_ptr() can return non-null while rt->heap is still
  // garbage -- dereferencing it then crashes. This matters because liballocs (the
  // preload) starts routing metadata queries here the moment its lazy dlsym resolves
  // our symbols, and the first such queries arrive while libalaska itself is still
  // being mmap'd/loaded. is_initialized() reads a plain volatile bool, so it is safe
  // to call at any point; gate every heap-touching accessor on it.
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  return rt->heap.pm.get_start();
}

// Size in bytes of Alaska's contiguous backing heap.
extern "C" ALASKA_EXPORT unsigned long alaska_heap_size(void) {
  return (unsigned long)alaska::heap_size;
}

// Given a raw BACKING pointer (interior pointers allowed), return the start of
// the object that contains it, or NULL if the pointer is in neither Alaska's
// sized heap nor a huge object's mmap.
extern "C" ALASKA_EXPORT void *alaska_object_base(void *backing) {
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  alaska::HeapPage *page = rt->heap.pt.get_unaligned(backing);
  if (page != nullptr) return page->object_base(backing);
  // Not in the sized heap: maybe a huge object (separate mmap, not in the page table).
  return rt->heap.huge_allocator.object_base(backing);
}

// Inverse of alaska_translate. For debug puposes only, awfully slow.
extern "C" ALASKA_EXPORT void *alaska_handle_for(void *backing) {
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  alaska::HeapPage *page = rt->heap.pt.get_unaligned(backing);
  if (page == nullptr) return nullptr;  // not in the sized heap (maybe huge)
  alaska::Mapping *m = page->mapping_of(backing);
  if (m == nullptr) return nullptr;  // free slot
  void *base = page->object_base(backing);
  uint32_t offset = (uint32_t)((uintptr_t)backing - (uintptr_t)base);
  return m->to_handle(offset);
}

// Size liballocs should use for the object containing a raw BACKING pointer, or
// 0 if it owns neither a sized- nor huge-heap object there. This is the heap
// slot size (caller request rounded up, PLUS the trailing-insert reserve) -- NOT
// the exact requested size; liballocs' get_info overrides it with the exact size
// from the side table, but insert_for_chunk needs the slot size so the insert
// lands on the reserved tail. NOTE: this works on the backing pointer directly --
// unlike alaska_usable_size(), which expects a *handle*.
extern "C" ALASKA_EXPORT unsigned long alaska_object_size(void *backing) {
  if (!alaska::is_initialized()) return 0;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  alaska::HeapPage *page = rt->heap.pt.get_unaligned(backing);
  if (page != nullptr) return (unsigned long)page->size_of(backing);
  // Not in the sized heap: maybe a huge object. backing_size_of() already folds
  // in the insert reserve, mirroring the sized page's slot size.
  return (unsigned long)rt->heap.huge_allocator.backing_size_of(backing);
}

// For a raw BACKING pointer that lands in an *allocated* Alaska heap page,
// report the extent [base, base+size) of the containing page and return 1;
// return 0 otherwise. liballocs uses this to lazily claim that page as a
// bigalloc the first time a query for it misses, so queries route to the
// Alaska allocator. Page-granular (not whole-heap) so each bigalloc stays well
// under liballocs' max-bigalloc size and nesting stays simple.
extern "C" ALASKA_EXPORT int alaska_heap_page_extent(
    void *backing, void **out_base, unsigned long *out_size) {
  if (!alaska::is_initialized()) return 0;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  void *start = rt->heap.pm.get_start();
  if (backing >= start) {
    uintptr_t off = (uintptr_t)backing - (uintptr_t)start;
    if (off < alaska::heap_size) {
      // Only claim pages that actually back a live sized object.
      if (rt->heap.pt.get_unaligned(backing) == nullptr) return 0;
      uintptr_t page_ind = off / alaska::page_size;
      if (out_base) *out_base = (void *)((uintptr_t)start + page_ind * alaska::page_size);
      if (out_size) *out_size = (unsigned long)alaska::page_size;
      return 1;
    }
  }
  // Outside the sized heap: a huge object lives in its own mmap region. Report
  // that whole region so liballocs claims it as one bigalloc; the per-object
  // base/size then comes from alaska_object_base/size above.
  return rt->heap.huge_allocator.extent_of(backing, out_base, out_size) ? 1 : 0;
}
