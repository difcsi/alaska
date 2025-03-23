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

#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <alaska/alaska.hpp>
#include <alaska/Logger.hpp>
#include <alaska/HeapPage.hpp>

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
  static constexpr uint64_t locality_slab_shift_factor = 13;  //
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
  struct LocalitySlab final {
    size_t bump_size = 0;  // How many bytes have been allocated in this slab
    size_t freed = 0;      // how many bytes have been freed.
    uint8_t data[0];


    struct Metadata {
      uint16_t size : 16;  // Size of the object in bytes.
      uint64_t hid : 48;   // The handle ID
      char data[0];
    } __attribute__((packed));

    static_assert(sizeof(Metadata) == 8);
    void *alloc(size_t size, alaska::Mapping &m);
    void free(void *ptr);
    inline void *start(void) const { return (void *)((uintptr_t)this); }
    inline void *end(void) const { return (void *)((uintptr_t)this + locality_slab_size); }
    inline size_t available(void) const {
      return (uintptr_t)end() - (uintptr_t)start() - bump_size;
    }
    size_t get_size(void *ptr);  // must be the poitner to the start of the data.
  };

  // A locality page is meant to strictly bump allocate objects of variable size in order
  // of their expected access pattern in the future. It's optimized for moving objects into
  // this page, and the expected lifetimes of these objects is long enough that we don't really
  // care about freeing or re-using the memory occupied by them when they are gone.
  class LocalityPage final : public alaska::HeapPage {
   public:
    struct Metadata {
      union {
        alaska::Mapping *mapping;
        void *data_raw;
      };
      bool allocated;
      uint32_t size;

      void *get_data(void) const {
        if (not allocated) return data_raw;
        return mapping->get_pointer();
      }
    };

    LocalityPage(void *backing_memory)
        : alaska::HeapPage(backing_memory) {
      data = backing_memory;
      data_bump_next = data;
      md_bump_next = get_md(0);
    }

    ~LocalityPage() override;

    void *alloc(const alaska::Mapping &m, alaska::AlignedSize size) override;
    bool release_local(const alaska::Mapping &m, void *ptr) override;
    size_t size_of(void *) override;
    inline size_t available() const { return get_free_space() - sizeof(Metadata); }


    void dump_html(FILE *stream) override;
    void dump_json(FILE *stream) override;

    bool should_localize_from(uint64_t current_epoch) const override {
      return false;
      return current_epoch - last_localization_epoch > localization_epoch_hysteresis;
    }


    inline size_t heap_size(void) const { return (uint64_t)data_bump_next - (uint64_t)data; }

    inline float utilization(void) const {
      auto heap_bytes = heap_size();
      return (heap_bytes - bytes_freed) / (float)heap_bytes;
    }


    size_t compact(void);

   private:
    Metadata *find_md(void *ptr);
    inline Metadata *get_md(uint32_t offset) {
      return (Metadata *)((uintptr_t)data + page_size) - (offset + 1);
    }

    inline void *get_ptr(uint32_t index) { return get_md(index)->get_data(); }

    inline int num_allocated(void) { return get_md(0) - (md_bump_next); }

    inline size_t get_free_space() const { return (off_t)md_bump_next - (off_t)data_bump_next; }
    inline size_t used_space() const { return (off_t)data_bump_next - (off_t)data; }



    void *data = nullptr;
    void *data_bump_next = nullptr;
    Metadata *md_bump_next = nullptr;
    uint64_t bytes_freed = 0;

   public:
    uint64_t last_localization_epoch = 0;
    uint64_t localization_epoch_hysteresis = 10;

    LocalitySlab slabs[locality_slabs];
  };
};  // namespace alaska
