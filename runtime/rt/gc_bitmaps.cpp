/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Stackscan "present" side bitmap -- see alaska/gc_bitmaps.hpp.
 *
 * One bit per handle-table slot, marking handles found on a thread's stack
 * during a stop-the-world barrier. The reclamation pass (see refcount.cpp,
 * reclaim_dead_handles) frees a zero-refcount handle only if its present bit is
 * clear (not on any stack). present_clear runs on the barrier thread before the
 * barrier; present_mark runs in the barrier signal handler on each participating
 * thread (atomic OR, async-signal-safe); present_test runs in the barrier
 * callback while the world is stopped.
 */

#include <alaska/gc_bitmaps.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/HandleTable.hpp>
#include <alaska/liballoc.h>
#include <stdint.h>
#include <string.h>

namespace alaska::gc {

  unsigned long g_mark_count = 0;  // diagnostic: total present_mark calls that hit

  namespace {

    // One flat bitmap, one bit per handle-table slot.
    struct Bitmap {
      uint64_t *words = nullptr;  // preallocated; never realloc'd while a scan runs
      size_t nwords = 0;          // allocated word count
      size_t nbits = 0;           // active bit capacity for this cycle
      uintptr_t base = 0;         // handle-table base captured at clear() time
    };

    Bitmap g_present;

    // Number of mapping slots the handle table can currently address.
    inline size_t table_slot_count(void) {
      auto &ht = alaska::Runtime::get().handle_table;
      return ht.capacity() * alaska::HandleTable::slab_capacity;
    }

    // Resize (if needed) and zero a bitmap to cover the current table capacity.
    // Runs on the barrier thread before the barrier -- allocation here is fine.
    void clear(Bitmap &b) {
      auto &ht = alaska::Runtime::get().handle_table;
      b.base = (uintptr_t)ht.get_base();
      size_t need_bits = table_slot_count();
      size_t need_words = (need_bits + 63) / 64;
      if (need_words > b.nwords) {
        uint64_t *nw = (uint64_t *)alaska_internal_calloc(need_words, sizeof(uint64_t));
        if (b.words) alaska_internal_free(b.words);
        b.words = nw;
        b.nwords = need_words;
      } else if (b.words) {
        memset(b.words, 0, b.nwords * sizeof(uint64_t));
      }
      // Publish the new bit capacity last, after storage/base are in place, so a
      // concurrent mark() either sees the old (cleared) state or the new one.
      __atomic_store_n(&b.nbits, need_bits, __ATOMIC_RELEASE);
    }

    // Flat slot index of a mapping, or -1 if outside the cycle's bitmap. A
    // mapping from a slab that grew mid-cycle lands out of range and is treated
    // conservatively (mark no-ops; test reports "present").
    inline long slot_index(const Bitmap &b, alaska::Mapping *m) {
      uintptr_t p = (uintptr_t)m;
      uintptr_t base = b.base;
      size_t nbits = __atomic_load_n(&b.nbits, __ATOMIC_ACQUIRE);
      if (p < base) return -1;
      size_t idx = (p - base) / sizeof(alaska::Mapping);
      if (idx >= nbits) return -1;
      return (long)idx;
    }

    inline void mark(Bitmap &b, alaska::Mapping *m) {
      long i = slot_index(b, m);
      if (i < 0) return;  // out of range: conservatively un-markable this cycle
      uint64_t *word = &b.words[(size_t)i >> 6];
      uint64_t bit = 1ULL << ((size_t)i & 63);
      __atomic_fetch_or(word, bit, __ATOMIC_RELEASE);
      __atomic_fetch_add(&g_mark_count, 1, __ATOMIC_RELAXED);
    }

    // Out-of-range slots report `true` so the collector never frees a handle it
    // could not represent this cycle.
    inline bool test(const Bitmap &b, alaska::Mapping *m) {
      long i = slot_index(b, m);
      if (i < 0) return true;
      uint64_t word = __atomic_load_n(&b.words[(size_t)i >> 6], __ATOMIC_ACQUIRE);
      return (word >> ((size_t)i & 63)) & 1;
    }

  }  // namespace

  void present_clear(void) { clear(g_present); }
  void present_mark(alaska::Mapping *m) { mark(g_present, m); }
  bool present_test(alaska::Mapping *m) { return test(g_present, m); }

}  // namespace alaska::gc

// --- C entry points ---------------------------------------------------------
extern "C" {

void alaska_gc_present_clear(void) { alaska::gc::present_clear(); }
void alaska_gc_present_mark(void *handle) {
  auto *m = alaska::Mapping::from_handle_safe(handle);
  if (m) alaska::gc::present_mark(m);
}
int alaska_gc_present_test(void *handle) {
  auto *m = alaska::Mapping::from_handle_safe(handle);
  return m ? (int)alaska::gc::present_test(m) : 1;  // non-handle: conservatively present
}

unsigned long alaska_gc_present_mark_count(void) {
  return __atomic_load_n(&alaska::gc::g_mark_count, __ATOMIC_RELAXED);
}

}  // extern "C"
