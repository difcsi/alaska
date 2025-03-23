
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
    ck::set<uintptr_t> pages;
    ck::vec<alaska::Mapping *> to_move;
    to_move.ensure_capacity(count);

    auto start = alaska_timestamp();

    // first, compute utilization
    for (size_t i = 0; i < count; i++) {
      auto hid = handle_ids[i];
      if (hid == 0) continue;
      auto handle = reinterpret_cast<void *>((1LU << 63) | ((uint64_t)hid << ALASKA_SIZE_BITS));
      auto *m = alaska::Mapping::from_handle(handle);
      if (m->get_pointer() == NULL) continue;
      auto size = tc.get_size(handle);
      htlb_reach += size;
      pages.add((uint64_t)m->get_pointer() >> 12);

      if (size < 4096 && !m->is_free() && !m->is_pinned()) {
        to_move.push(m);
      }
    }


    size_t required_pages = (htlb_reach + 4096 - 1) / 4096;
    size_t used_pages = pages.size();
    float utilization = required_pages / (float)used_pages;
    float util_after = utilization;

    pages.clear();

    if (utilization < 0.6) {
      tc.localize(to_move.data(), to_move.size());
      // for (auto *m : to_move) {
      //   bool moved = tc.localize(*m, rt.localization_epoch);
      //   if (moved) {
      //     moved_objects++;
      //   } else {
      //     unmoved_objects++;
      //   }
      //   pages.add((uint64_t)m->get_pointer() >> 12);
      // }
      // size_t used_pages = pages.size();
      // util_after = required_pages / (float)used_pages;
    }
    auto end = alaska_timestamp();


    if (util_after > utilization) {
      // make the text green
      printf("\033[0;32m");
    } else {
      // make the text red
      printf("\033[0;31m");
    }

    printf(
        "[localization] required: %12zu, used: %12zu,   util: %.4f -> %.4f,   moved: %3d/%3d, "
        "time: "
        "%12luus\033[0m\n",
        required_pages, used_pages, utilization, util_after, moved_objects,
        moved_objects + unmoved_objects, (end - start) / 1000);


    // Push the buffer back to the queue of buffers
    struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    buf->next = buffers;
    buffers = buf;
  }
}  // namespace alaska
