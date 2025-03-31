#pragma once

#include <alaska/sim/TLB.hpp>
#include <alaska/sim/StatisticsManager.hpp>
#include <alaska/ThreadCache.hpp>
#include <cstdint>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <alaska/Runtime.hpp>
#include <vector>
#include <iostream>
#include "alaska/HeapPage.hpp"

namespace alaska::sim {



  /**
   * @brief A simple associative cache implementation
   * @tparam T The type of the tag
   * @details This is a simple associative cache implementation that uses LRU
   * replacement policy. It is used to implement the L1 and L2 levels in the HTLB
   * implementation.
   *
   * It does not store the actual data, only the tags and the last access time.
   */
  template <typename T>
  class AssociativeCache {
   public:
    using TagType = T;

    uint64_t misses = 0;
    uint64_t hits = 0;

    struct Entry {
      TagType tag;
      uint64_t last_accessed;
      bool valid;

      Entry()
          : tag(0)
          , last_accessed(0)
          , valid(false) {}  // Default invalid entry

      Entry &operator=(const Entry &other) {
        if (this != &other) {
          tag = other.tag;
          last_accessed = other.last_accessed;
          valid = other.valid;
        }
        return *this;
      }
    };

    AssociativeCache(int sets, int ways)
        : num_sets(sets)
        , num_ways(ways)
        , access_counter(0) {
      cache.resize(sets, std::vector<Entry>(ways));  // Preallocate entries as invalid
    }

    void reset(void) {
      for (auto &set : cache) {
        for (auto &entry : set) {
          entry.valid = false;
        }
      }
      hits = 0;
      misses = 0;
      access_counter = 0;
    }

    bool access(TagType tag) {
      TagType set_idx = tag % num_sets;

      auto &set = cache[set_idx];
      for (auto &entry : set) {
        if (entry.valid && entry.tag == tag) {
          entry.last_accessed = access_counter++;
          hits++;
          return true;
        }
      }
      misses++;
      return false;
    }

    bool insert(TagType tag, TagType &evicted) {
      TagType set_idx = tag % num_sets;

      auto &set = cache[set_idx];

      // Look for an invalid entry first
      for (auto &entry : set) {
        if (!entry.valid) {
          entry.tag = tag;
          entry.last_accessed = access_counter++;
          entry.valid = true;
          return false;
        }
      }

      // Otherwise, evict LRU entry
      auto lru_it = std::min_element(set.begin(), set.end(), [](const Entry &a, const Entry &b) {
        return a.last_accessed < b.last_accessed;
      });

      evicted = lru_it->tag;
      lru_it->tag = tag;
      lru_it->last_accessed = access_counter++;
      lru_it->valid = true;
      return true;
    }

    void invalidate(TagType tag) {
      TagType set_idx = tag % num_sets;

      auto &set = cache[set_idx];
      for (auto &entry : set) {
        if (entry.valid && entry.tag == tag) {
          entry.valid = false;  // Mark entry as invalid
          return;
        }
      }
    }

    int num_sets, num_ways;
    uint64_t access_counter;
    std::vector<std::vector<Entry>> cache;
  };


  template <typename T>
  class TwoLevelCache {
   public:
    using TagType = T;
    TwoLevelCache(int l1_sets, int l1_ways, int l2_sets, int l2_ways)
        : l1(l1_sets, l1_ways)
        , l2(l2_sets, l2_ways)
        , hits(0)
        , misses(0) {}

    int total_entries(void) { return l1.num_sets * l1.num_ways + l2.num_sets * l2.num_ways; }

    void reset(void) {
      l1.reset();
      l2.reset();
      hits = 0;
      misses = 0;
    }


    std::vector<TagType> get_entries() {
      std::vector<TagType> entries;
      for (const auto &set : l1.cache) {
        for (const auto &entry : set) {
          if (entry.valid) {
            entries.push_back(entry.tag);
          }
        }
      }
      for (const auto &set : l2.cache) {
        for (const auto &entry : set) {
          if (entry.valid) {
            entries.push_back(entry.tag);
          }
        }
      }
      return entries;
    }

    // access the cache. Return true if the access was a hit, false otherwise.
    bool access(TagType addr) {
      if (l1.access(addr)) {
        // printf("hit in l1\n");
        hits++;
        return true;
      }

      if (l2.access(addr)) {
        TagType evicted;

        if (l1.insert(addr, evicted)) {
          // printf("hit in l2. l1 victim=%d\n", evicted);
          l2.invalidate(addr);
          l2.insert(evicted, evicted);
        } else {
          // printf("hit in l2. l1 no victim\n");
        }
        hits++;
        return true;
      }

      TagType evicted;
      bool evicted_from_l1 = l1.insert(addr, evicted);

      if (evicted_from_l1) {
        TagType evicted_l2;
        // printf("inserted %d into l1. victim=%d\n", addr, evicted);
        l2.insert(evicted, evicted_l2);
      } else {
        // printf("inserting %d into l2. no victim\n", addr);
      }
      misses++;
      return false;
    }


    void print_state() {
      auto dump_cache = [](const auto &cache) {
        auto accesses = cache.hits + cache.misses;
        auto hit_rate = accesses == 0 ? 0 : (double)cache.hits / accesses * 100;
        printf(
            "   hits: %12zu, misses: %12zu, rate: %4.1f%%\n", cache.hits, cache.misses, hit_rate);
        int set_idx = 0;
        for (const auto &set : cache.cache) {
          set_idx++;
          // printf("   set %2d: ", set_idx++);
          for (const auto &entry : set) {
            if (entry.valid) {
              printf("%9lx ", (uint64_t)entry.tag);
            } else {
              printf("_________ ");
            }
          }
          if (set_idx % 2 == 0) {
            printf("\n");
          } else {
            printf("   |   ");
          }
        }
        printf("\n");
      };
      printf("  l1 entries:\n");
      dump_cache(l1);
      printf("  l2 entries:\n");
      dump_cache(l2);
    }

    AssociativeCache<TagType> l1, l2;
    uint64_t hits, misses;
  };


  class HTLB {
   public:
    using page_t = uintptr_t;
    std::vector<alaska::handle_id_t> full_trace;
    TwoLevelCache<alaska::handle_id_t> htlb;

    TwoLevelCache<page_t> tlb;
    uint64_t handle_walks = 0;
    uint64_t page_walks = 0;
    uint64_t memory_accesses = 0;

    ThreadCache *thread_cache = nullptr;


    HTLB();
    static HTLB *get();


    void reset() {
      htlb.reset();
      tlb.reset();
      full_trace.clear();
      handle_walks = 0;
      page_walks = 0;
      memory_accesses = 0;
    }

    template <typename T>
    page_t to_page(T *addr) {
      return (page_t)((uintptr_t)addr >> 12);
    }

    void print_state() {
      printf("memory accesses: %lu\n", memory_accesses);
      printf("handle walks:    %lu\n", handle_walks);
      printf("page walks:      %lu\n", page_walks);
      printf("HTLB\n");
      htlb.print_state();
      printf("TLB\n");
      tlb.print_state();
    }

    // Localize, returning if utilization has been improved.
    bool localize(void) {
      auto &rt = alaska::Runtime::get();

      auto utilization_before = get_htlb_utilization();

      static int localization_count = 0;

      rt.with_barrier([&]() {
        auto handles = htlb.get_entries();
        this->thread_cache->localize(handles.data(), handles.size());


        auto &ht = rt.handle_table;
        if (localization_count++ % 50 == 0) {
          int hot_cutoff = 8;
          size_t hot_bytes = 0;
          size_t cold_bytes = 0;

          size_t total_heap_sw_pages = rt.heap.pm.get_allocated_page_count();
          size_t total_heap_hw_pages = total_heap_sw_pages * (alaska::page_size / 4096);

          ck::set<uintptr_t> hot_pages;
          ck::set<uintptr_t> hot_handle_pages;
          ck::set<uintptr_t> cold_pages;

          auto slabs = ht.get_slabs();
          for (auto *slab : slabs) {
            for (auto *allocated : slab->allocator) {
              auto *m = (alaska::Mapping *)allocated;
              if (m->get_pointer() == nullptr) continue;
              uintptr_t page = (uintptr_t)m->get_pointer() >> 12;
              auto header = alaska::ObjectHeader::from(m->get_pointer());
              if (header->hotness > hot_cutoff) {
                hot_handle_pages.add((uintptr_t)m >> 12);
                hot_bytes += header->object_size();
                hot_pages.add(page);
              } else {
                cold_bytes += header->object_size();
                cold_pages.add(page);
              }
            }
          }

          auto total_bytes = hot_bytes + cold_bytes;
          auto pages_needed_for_hot_objects = round_up(hot_bytes, 4096) / 4096;
          float hot_utilization = pages_needed_for_hot_objects / (float)hot_pages.size();

          printf(
              "[dump %5zu] hot/cold pages: %5zu(%4zu needed %5f)/%5zu | hot/cold bytes: "
              "%12zu/%12zu | %5zu hot handle pages\n",
              localization_count, hot_pages.size(), pages_needed_for_hot_objects, hot_utilization,
              cold_pages.size(), hot_bytes, cold_bytes, hot_handle_pages.size());
        }
        // printf("BARRIER\n");
      });

      auto utilization_after = get_htlb_utilization();
      // printf("%3.0f%% -> %3.0f%%\n", utilization_before * 100.0, utilization_after * 100.0);
      // rt.heap.dump(stdout);
      // we want to raise utilization by localization. Return true if we have.
      return utilization_after > utilization_before;
    }


    float get_htlb_utilization(void) {
      auto entries = htlb.get_entries();
      size_t htlb_reach = 0;
      std::unordered_set<page_t> pages;
      for (auto hid : entries) {
        auto m = alaska::Mapping::from_handle_id(hid);
        auto page = to_page((char *)m->get_pointer());
        pages.insert(page);
        auto size = this->thread_cache->get_size(m->to_handle());
        htlb_reach += size + sizeof(alaska::ObjectHeader);
      }
      size_t required_pages = (htlb_reach + 4096 - 1) / 4096;
      size_t used_pages = pages.size();
      float utilization = required_pages / (float)used_pages;
      return utilization;
    }

    void access(alaska::Mapping &m, uint32_t offset) {
      bool hit_in_htlb = htlb.access(m.handle_id());
      if (!hit_in_htlb) {
        // NOTE: the handle table walk does *not* occur in
        // virtual memory, so we don't have to access the tlb
        // to perform a handle table walk.
        handle_walks++;
        memory_accesses++;
      }

      bool hit_in_tlb = tlb.access(to_page((char *)m.get_pointer() + offset));
      if (!hit_in_tlb) {
        memory_accesses += 3;  // page walk!
        page_walks++;
      }
      memory_accesses++;  // the actual access.



      // if (!hit_in_tlb) {
      //   auto entries = htlb.get_entries();
      //   size_t htlb_reach = 0;
      //   std::unordered_set<page_t> pages;
      //   std::unordered_set<size_t> sizes;
      //   for (auto hid : entries) {
      //     auto m = alaska::Mapping::from_handle_id(hid);
      //     auto page = to_page((char *)m->get_pointer());
      //     pages.insert(page);
      //     auto size = this->thread_cache->get_size(m->to_handle());
      //     sizes.insert(size);
      //     htlb_reach += size;
      //   }
      //   size_t required_pages = (htlb_reach + 4096 - 1) / 4096;
      //   size_t used_pages = pages.size();
      //   float utilization = required_pages / (float)used_pages;
      //   printf(
      //       "[localization] ents: %12zu, required: %12zu, used: %12zu,   util: %.4f,   sizes: "
      //       "%12zu:  ",
      //       entries.size(), required_pages, used_pages, utilization, sizes.size());
      //   for (auto s : sizes) {
      //     printf("%zu ", s);
      //   }
      //   printf("\n");
      // }

      // printf("HTLB\n");
      // htlb.print_state();
      // printf("TLB\n");
      // tlb.print_state();
      // usleep(200000);
    }
  };


}  // namespace alaska::sim
