
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


#include <alaska/Runtime.hpp>
#include <alaska/Localizer.hpp>
#include <alaska/lphash_set.h>

#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS
#include <math.h>

namespace alaska {



  Localizer::Localizer(alaska::Configuration &config, alaska::ThreadCache &tc)
      : tc(tc) {
    if (getenv("HOT_CUTOFF") != NULL) {
      knobs.hotness_cutoff = atoi(getenv("HOT_CUTOFF"));
    }
  }

  handle_id_t *Localizer::get_hotness_buffer(size_t count) {
    if (expected_count == 0) {
      expected_count = count;
    } else {
      ALASKA_ASSERT(expected_count == count, "Localizer count mismatch");
    }
    void *buf = NULL;

    if (buffers == NULL) {
      buf = alaska_internal_malloc(count * sizeof(handle_id_t));
    } else {
      buf = (void *)buffers;
      buffers = buffers->next;
    }
    return reinterpret_cast<handle_id_t *>(buf);
  }


  float Localizer::compute_quality(handle_id_t *hids, size_t count) {
    size_t bytes_needed = 0;
    size_t bytes_used = 0;
    uintptr_t pages[count];
    lphashset_init(pages, count);

    for (size_t i = 0; i < count; i++) {
      if (hids[i] == 0) continue;
      auto *m = Mapping::from_handle_id(hids[i]);
      void *data = m->get_pointer();
      if (data == nullptr) continue;
      auto header = ObjectHeader::from(m);
      size_t object_size = header->object_size();
      // sizes.push(object_size);
      bytes_needed += object_size + sizeof(ObjectHeader);

      if (lphashset_insert(pages, count, (uintptr_t)data >> 12)) {
        bytes_used += 0x1000;
      }
    }

    // Quality is a metric we utilize to determine if objects in a
    // dump are at a good location or not.  It is based on the
    // assumption that in order to have good locality, the first step
    // is to minimize the number of pages used by a given set of
    // objects. Therefore, we count quality as a number between 0 and
    // 1, where 0 is bad quality, and 1 is good. If the quality is bad
    // (say 0.1), that means a set of objects is using 10x the number
    // of pages it should be. If the quality is good (0.9+), the
    // objects are using near the minimal number of pages.  A set of
    // objects with horrible quality should be more likley to be
    // localized, as they were observed in a dump together, and we
    // ought to optimize for that pattern happening again (assuming it
    // does, anyways).
    float quality = ((float)bytes_needed / (float)bytes_used);
    if (quality < 0) {
      quality = 0;
    }

    return quality;
  }

  Localizer::ScanResult Localizer::feed_hotness_buffer(size_t count, handle_id_t *handle_ids) {
    ScanResult res = {0};
    res.localized = false;
    dumps_recorded++;

    auto &rt = alaska::Runtime::get();


    alaska::printf("YUKON_DUMP: %zu", alaska_timestamp());
    for (size_t i = 0; i < count; i++) {
      if (handle_ids[i] == 0) continue;

      // check if it is valid or not.
      auto *m = Mapping::from_handle_id(handle_ids[i]);
      void *data = m->get_pointer();
      if (data == nullptr || m->is_free()) continue;
      auto header = ObjectHeader::from(m);
      alaska::printf(" %p:%zu", m->get_pointer(), header->object_size());
    }
    alaska::printf("\n");

    // // Push the buffer back to the queue of buffers
    // struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    // buf->next = buffers;
    // buffers = buf;

    // return res;

#if 1

    float quality = compute_quality(handle_ids, count);
    // alaska::printf("YUKON_QUALITY=%f\n", quality);
    quality = 0;


    float quality_cutoff = 0.15;
    int inc = (1 - (quality / quality_cutoff)) * 8;
    inc = knobs.effort * 16;

    this->last_quality = quality;

    // TODO: can we sort by data address?


    for (size_t i = 0; i < count; i++) {
      // If the handle is 0, skip it. This means the HTLB entry was empty at the
      // time of the dump (invalidation, etc)
      if (handle_ids[i] == 0) continue;
      auto *m = Mapping::from_handle_id(handle_ids[i]);
      void *data = m->get_pointer();
      if (data == nullptr || m->is_free()) continue;
      auto header = ObjectHeader::from(m);

      if (header->localized) {
        // If the object is already localized, skip it.
        continue;
      }

      int hotness = header->hotness;
      bool was_hot = hotness > knobs.hotness_cutoff;
      hotness += 1;
      bool is_hot = hotness > knobs.hotness_cutoff;

      if (not was_hot and is_hot) {
        saturated_bytes += header->object_size() + sizeof(ObjectHeader);
        saturated_handles.push(m->handle_id());
      }

      header->hotness = hotness;
      if (hotness > 0b111111) {
        hotness = 0b111111;
      }
    }

    bool should_localize = false;
    if (dumps_recorded > knobs.localization_interval) should_localize = true;



    if (should_localize) {
      alaska::printf("Localizing! %zu in queue\n", saturated_handles.size());
      uint64_t scanned_hot = 0;
      dumps_recorded = 0;
      res.localized = true;

      localize_saturated_handles();

      rt.heap.compact_locality_pages();
      // rt.heap.compact_sizedpages();

      saturated_bytes = 0;
      saturated_handles.clear();
    }


    // Push the buffer back to the queue of buffers
    struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    buf->next = buffers;
    buffers = buf;

    return res;
#endif
  }



  void Localizer::localize_saturated_handles() {
    auto &rt = alaska::Runtime::get();

    bool did_localize = rt.with_barrier([&] {
      // iterate over our queue of saturated handles from start to end, and localize those which are
      // valid. NOTE: this does not clear the queue, just localizes them.
      for (auto &handle : saturated_handles) {
        auto *m = Mapping::from_handle_id(handle);
        if (rt.handle_table.valid_handle(m)) {
          auto headerBefore = ObjectHeader::from(m);
          size_t objectSize = headerBefore->object_size();
          // if the handle is valid, localize it
          this->tc.localize(m, 0);
          bytes_localized_counter.track(objectSize);
          // printf("localized %p %zu \n", m, object_size);
          localized_objects += objectSize;
        }
      }
    });
    if (!did_localize) {
      printf("Failed to localize\n");
    }
  }
}  // namespace alaska
