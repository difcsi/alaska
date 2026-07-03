/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * liballocs integration: raw-pointer metadata helpers (ported from main-rc, adapted to dev).
 *
 * These functions let an external liballocs (loaded as a separate DSO / LD_PRELOAD) answer
 * metadata queries (base/size/handle) for objects living in Alaska's backing heap. They
 * operate purely on RAW BACKING pointers -- the kind liballocs holds after stackscan's
 * translate-then-index -- and never inspect the handle bit; handles are an Alaska concept and
 * liballocs stays handle-agnostic. The symbols are exported with default visibility so
 * liballocs can weak-import them at load time.
 *
 * Every accessor gates on is_initialized() (a plain volatile-bool read, safe at any time):
 * liballocs starts routing queries the moment its lazy dlsym resolves our symbols, which can
 * be while libalaska itself is still being mmap'd. It also bounds-checks with heap.contains()
 * BEFORE Heap::get_page(), because get_page() reads the page header at the aligned address
 * unconditionally -- a raw out-of-heap pointer would fault otherwise.
 *
 * PORT-NOTE (dev vs main-rc): main-rc resolved huge objects (separate mmaps) too, via
 * huge_allocator.{object_base,backing_size_of,extent_of}. dev's HugeObjectAllocator only
 * exposes size_of(ptr) and no interior->base / extent lookup, so the huge path is NOT
 * resolved here (returns "unknown"). Sized-heap objects -- the common case -- are fully
 * resolved via HeapPage::object_base/size_of/mapping_of (SizedPage overrides). The
 * per-allocation insert reserve is ALASKA_LIBALLOCS_INSERT_RESERVE (0 in a standalone build).
 */

#include <alaska/core/Runtime.hpp>
#include <alaska/heaps/Heap.hpp>
#include <alaska/heaps/HeapPage.hpp>
#include <alaska/alaska.hpp>
#include <stddef.h>
#include <stdint.h>

#define ALASKA_EXPORT __attribute__((visibility("default")))

// Start of Alaska's contiguous backing heap, or NULL before the runtime exists.
extern "C" ALASKA_EXPORT void *alaska_heap_start(void) {
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return nullptr;
  return rt->heap.get_start();
}

// Size in bytes of Alaska's contiguous backing heap.
extern "C" ALASKA_EXPORT unsigned long alaska_heap_size(void) {
  return (unsigned long)alaska::Heap::heap_size;
}

// Given a raw BACKING pointer (interior allowed), return the start of the containing
// object, or NULL if it is not in Alaska's sized heap.
extern "C" ALASKA_EXPORT void *alaska_object_base(void *backing) {
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr || !rt->heap.contains(backing)) return nullptr;
  alaska::HeapPage *page = alaska::Heap::get_page(backing);
  return (page != nullptr) ? page->object_base(backing) : nullptr;
}

// Slot size of the object containing a raw BACKING pointer, or 0 if not in the sized heap.
extern "C" ALASKA_EXPORT unsigned long alaska_object_size(void *backing) {
  if (!alaska::is_initialized()) return 0;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr || !rt->heap.contains(backing)) return 0;
  alaska::HeapPage *page = alaska::Heap::get_page(backing);
  return (page != nullptr) ? (unsigned long)page->size_of(backing) : 0;
}

// Inverse of alaska_translate: the handle (with interior offset folded in) for a raw
// BACKING pointer, or NULL if it is not a live sized-heap object. Debug/diagnostic use.
extern "C" ALASKA_EXPORT void *alaska_handle_for(void *backing) {
  if (!alaska::is_initialized()) return nullptr;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr || !rt->heap.contains(backing)) return nullptr;
  alaska::HeapPage *page = alaska::Heap::get_page(backing);
  if (page == nullptr) return nullptr;
  alaska::Mapping *m = page->mapping_of(backing);
  if (m == nullptr) return nullptr;  // free slot
  void *base = page->object_base(backing);
  uint32_t offset = (base != nullptr) ? (uint32_t)((uintptr_t)backing - (uintptr_t)base) : 0;
  return m->to_handle(offset);
}

// For a raw BACKING pointer in an allocated sized page, report the extent
// [base, base+page_size) of the containing page and return 1; else 0. liballocs claims
// each such page as a bigalloc so queries route to the Alaska allocator.
extern "C" ALASKA_EXPORT int alaska_heap_page_extent(
    void *backing, void **out_base, unsigned long *out_size) {
  if (!alaska::is_initialized()) return 0;
  auto *rt = alaska::Runtime::get_ptr();
  if (rt == nullptr) return 0;
  void *start = rt->heap.get_start();
  if (start == nullptr || backing < start) return 0;
  uintptr_t off = (uintptr_t)backing - (uintptr_t)start;
  if (off >= alaska::Heap::heap_size) return 0;
  if (alaska::Heap::get_page(backing) == nullptr) return 0;  // no live sized object here
  uintptr_t page_ind = off / alaska::page_size;
  if (out_base) *out_base = (void *)((uintptr_t)start + page_ind * alaska::page_size);
  if (out_size) *out_size = (unsigned long)alaska::page_size;
  return 1;
}
