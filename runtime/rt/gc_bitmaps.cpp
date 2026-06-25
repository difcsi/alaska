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
#include <ck/lock.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

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

  // --- Nullcount bitmap -------------------------------------------------------
  // Unlike g_present (resized + zeroed every barrier on the barrier thread), this
  // bitmap is persistent and mutated by application threads at any time. We give it
  // a single, fixed allocation so set/clear never race a resize: the handle-table
  // base never moves (HandleTable::grow remaps in place), and we reserve enough bits
  // up front for any table the process will reach. MAP_NORESERVE keeps the resident
  // cost to just the words actually touched, so the large virtual reservation is
  // free in practice.
  namespace {
    // 2^28 slots -> 2^28/8 = 32 MiB of virtual address space, covering a handle
    // table of 256Ki slabs (~134M handles). Out-of-range handles (a table that
    // somehow grew past this) fall back to "untracked": set/clear no-op, so such a
    // handle is simply never bitmap-reclaimed -- safe, just a missed reclaim.
    constexpr size_t kNullcountSlots = 1UL << 28;

    struct NullcountBitmap {
      uint64_t *words = nullptr;  // published once, then never moved
      size_t nwords = 0;
      size_t nbits = 0;
      uintptr_t base = 0;  // handle-table base, captured at init (fixed thereafter)
    };
    NullcountBitmap g_nullcount;
    ck::mutex g_nullcount_init_lock;

    inline void nullcount_init(void) {
      if (likely(__atomic_load_n(&g_nullcount.words, __ATOMIC_ACQUIRE) != nullptr)) return;
      ck::scoped_lock l(g_nullcount_init_lock);
      if (g_nullcount.words != nullptr) return;  // lost the race; already up
      auto &ht = alaska::Runtime::get().handle_table;
      g_nullcount.base = (uintptr_t)ht.get_base();
      g_nullcount.nbits = kNullcountSlots;
      g_nullcount.nwords = (kNullcountSlots + 63) / 64;
      void *mem = mmap(nullptr, g_nullcount.nwords * sizeof(uint64_t), PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      ALASKA_ASSERT(mem != MAP_FAILED, "failed to reserve the nullcount bitmap");
      // Publish last: a racing reader either sees null (and inits) or the live array.
      __atomic_store_n(&g_nullcount.words, (uint64_t *)mem, __ATOMIC_RELEASE);
    }

    inline long nullcount_slot(alaska::Mapping *m) {
      uintptr_t p = (uintptr_t)m;
      if (p < g_nullcount.base) return -1;
      size_t idx = (p - g_nullcount.base) / sizeof(alaska::Mapping);
      if (idx >= g_nullcount.nbits) return -1;
      return (long)idx;
    }

    // Only the slots the handle table currently spans can ever be set, so a scan
    // need not walk the whole (mostly-zero) NORESERVE reservation. This keeps the
    // bitmap scan O(table size) -- comparable to the hashmap iterating its entries
    // -- instead of O(2^28). The barrier thread reads capacity() with the world
    // stopped, so no lock is needed.
    inline size_t nullcount_active_words(void) {
      if (g_nullcount.words == nullptr) return 0;
      auto &ht = alaska::Runtime::get().handle_table;
      size_t active_slots = (size_t)ht.capacity() * alaska::HandleTable::slab_capacity;
      size_t aw = (active_slots + 63) / 64;
      return aw < g_nullcount.nwords ? aw : g_nullcount.nwords;
    }
  }  // namespace

  void nullcount_bm_set(alaska::Mapping *m) {
    nullcount_init();
    long i = nullcount_slot(m);
    if (i < 0) return;
    __atomic_fetch_or(&g_nullcount.words[(size_t)i >> 6], 1ULL << ((size_t)i & 63), __ATOMIC_RELEASE);
  }

  void nullcount_bm_clear(alaska::Mapping *m) {
    if (__atomic_load_n(&g_nullcount.words, __ATOMIC_ACQUIRE) == nullptr) return;
    long i = nullcount_slot(m);
    if (i < 0) return;
    __atomic_fetch_and(
        &g_nullcount.words[(size_t)i >> 6], ~(1ULL << ((size_t)i & 63)), __ATOMIC_RELEASE);
  }

  void nullcount_bm_collect(ck::vec<alaska::Mapping *> &out) {
    uint64_t *words = __atomic_load_n(&g_nullcount.words, __ATOMIC_ACQUIRE);
    if (words == nullptr) return;
    size_t scan = nullcount_active_words();
    for (size_t w = 0; w < scan; w++) {
      uint64_t bits = __atomic_load_n(&words[w], __ATOMIC_ACQUIRE);
      while (bits) {
        size_t b = __builtin_ctzll(bits);
        bits &= bits - 1;  // clear lowest set bit
        size_t idx = w * 64 + b;
        out.push((alaska::Mapping *)(g_nullcount.base + idx * sizeof(alaska::Mapping)));
      }
    }
  }

  size_t nullcount_bm_count(void) {
    uint64_t *words = __atomic_load_n(&g_nullcount.words, __ATOMIC_ACQUIRE);
    if (words == nullptr) return 0;
    size_t scan = nullcount_active_words();
    size_t n = 0;
    for (size_t w = 0; w < scan; w++)
      n += __builtin_popcountll(__atomic_load_n(&words[w], __ATOMIC_ACQUIRE));
    return n;
  }

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
