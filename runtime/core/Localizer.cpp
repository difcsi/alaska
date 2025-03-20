
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
#include "alaska/liballoc.h"

namespace alaska {


  Localizer::Localizer(alaska::Configuration &config, alaska::ThreadCache &tc)
      : tc(tc) {}

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

  void Localizer::feed_hotness_buffer(size_t count, handle_id_t *handle_ids) {
    ALASKA_ASSERT(expected_count == count, "Localizer count mismatch");
    auto &rt = alaska::Runtime::get();

    unsigned long moved_objects = 0;
    unsigned long unmoved_objects = 0;
    unsigned long bytes_in_dump = 0;

    size_t htlb_reach = 0;
    size_t handles_seen = 0;


    ck::set<uintptr_t> pages;
    ck::map<size_t, int> size_hist;

    for (size_t i = 0; i < count; i++) {
      auto hid = handle_ids[i];
      if (hid == 0) continue;
      handles_seen++;
      auto handle = reinterpret_cast<void *>((1LU << 63) | ((uint64_t)hid << ALASKA_SIZE_BITS));
      auto *m = alaska::Mapping::from_handle(handle);
      auto size = tc.get_size(handle);
      htlb_reach += size;

      size_hist[size] += 1;

      pages.add((uint64_t)m->get_pointer() >> 12);
      continue;


      bool moved = false;
      if (m == NULL or m->is_free() or m->is_pinned()) {
        moved = false;
      } else {
        void *ptr = m->get_pointer();
        moved = tc.localize(*m, rt.localization_epoch);
      }
      if (moved) {
        moved_objects++;
        bytes_in_dump += size;
      } else {
        unmoved_objects++;
      }
    }


    size_t required_pages = (htlb_reach + 4096 - 1) /  4096;
    size_t used_pages = pages.size();

    // printf("handles in dump: %zu\n", handles_seen);

    printf("required: %zu, used: %zu, util: %f, %f\n", required_pages, used_pages, used_pages / (float)required_pages, required_pages / (float)used_pages);
    printf("size hist\n");
    for (auto &[size, count] : size_hist) {
      printf("%8zu: %d\n", size, count);
    }


    // Push the buffer back to the queue of buffers
    struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    buf->next = buffers;
    buffers = buf;
  }
}  // namespace alaska
