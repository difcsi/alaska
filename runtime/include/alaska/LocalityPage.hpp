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

#pragma once

#include <ck/vec.h>
#include <stdlib.h>
#include <stdint.h>
#include <alaska/alaska.hpp>
#include <alaska/Logger.hpp>
#include <alaska/HeapPage.hpp>
#include <alaska/SizedAllocator.hpp>

namespace alaska {

  /**
   * LocalityPage: a page which is used to allocate objects of variable sizes
   * in order of their expected access pattern in the future. We chunk the heap page
   * into "slabs" which are explicitly managed. Objects can only be allocated from
   * a locality page by a special mechanism (that is, the normal alloc function will
   * abort). Objects can be freed, but the memory will not be re-used until the page
   * is compacted (or evacutated).
   *
   *
   */

  // The power-of-two of a locality slab
  static constexpr uint64_t locality_slab_shift_factor = 13;
  // How many bytes are in a locality slab
  static constexpr uint64_t locality_slab_size = 1 << locality_slab_shift_factor;
  // How many slabs are in a locality page
  static constexpr uint64_t locality_slabs = 1 << (page_shift_factor - locality_slab_shift_factor);


  /**
   * LocalitySlab: a slice of the memory managed by a LocalityPage. This
   * structure gets a pointer to the start of the slab and strictly bump
   * allocates with no reuse policy.
   *
   * Each object allocated from the slab has a metadata header that is
   * used to track the size of the object in bytes.
   *
   * The LocalitySlab lives within the memory managed by the locality slab itself,
   * so it's important to keep it small to maximize the number of objects that can
   * be allocated from the slab.
   */
  struct LocalitySlab final : public alaska::InternalHeapAllocated {
    size_t bump_size = 0;        // How many bytes have been allocated in this slab
    size_t freed = 0;            // how many bytes have been freed.
    struct list_head slab_list;  // TODO: don't track like this.
    uint8_t data[0];


    void init(void) {
      bump_size = 0;
      freed = 0;
      slab_list = LIST_HEAD_INIT(slab_list);
    }
    void *alloc(size_t size, const alaska::Mapping &m);
    void free(void *ptr);
    inline void *start(void) const { return (void *)((uintptr_t)this); }
    inline void *end(void) const { return (void *)((uintptr_t)this + locality_slab_size); }
    inline size_t available(void) const {
      return (uintptr_t)end() - ((uintptr_t)start() + bump_size);
    }
    size_t get_size(void *ptr);  // must be the poitner to the start of the data.

    float fragmentation(void) { return (float)freed / (float)bump_size; }
    float utilization(void) { return (float)(bump_size - freed) / (float)locality_slab_size; }
  };

  // A locality page is meant to strictly bump allocate objects of variable size in order
  // of their expected access pattern in the future. It's optimized for moving objects into
  // this page, and the expected lifetimes of these objects is long enough that we don't really
  // care about freeing or re-using the memory occupied by them when they are gone.
  class LocalityPage final : public alaska::HeapPage {
   public:
    LocalityPage(void *backing_memory)
        : alaska::HeapPage(backing_memory) {
      locality_slab_allocator.configure(backing_memory, locality_slab_size, locality_slabs);
    }

    size_t available(void) {
      size_t remaining_in_current_slab = current_slab == NULL ? 0 : current_slab->available();
      return locality_slab_allocator.num_free() * locality_slab_size + remaining_in_current_slab;
    }


    LocalitySlab *get_slab(void *ptr) {
      off_t offset = (uintptr_t)ptr - (uintptr_t)this->memory;

      // now we have an offset within the page, we can grab the slab easily
      return (LocalitySlab *)((uintptr_t)this->memory + (offset & ~(locality_slab_size - 1)));
    }

    ~LocalityPage() override;

    void *alloc(const alaska::Mapping &m, alaska::AlignedSize size) override;
    bool release_local(const alaska::Mapping &m, void *ptr) override;


    // TODO: TEMPORARY
    struct list_head slab_list_head = LIST_HEAD_INIT(slab_list_head);
    template <typename T>
    void for_each_slab(T &&fn) {
      struct list_head *pos;
      list_for_each(pos, &slab_list_head) {
        auto *slab = list_entry(pos, LocalitySlab, slab_list);
        fn(slab);
      }
    }

   private:
    LocalitySlab *allocate_slab(void) {
      auto *slab = (LocalitySlab *)locality_slab_allocator.alloc();
      if (slab == nullptr) {
        return nullptr;
      }

      slab->init();
      // TODO: TEMPORARY
      list_add(&slab->slab_list, &slab_list_head);
      return slab;
    }
    LocalitySlab *current_slab = nullptr;
    SizedAllocator locality_slab_allocator;
  };
};  // namespace alaska
