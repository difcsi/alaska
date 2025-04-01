
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
#include <alaska/lphash_set.h>

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

    dumps_recorded++;

    for (size_t i = 0; i < count; i++) {
      if (handle_ids[i] == 0) continue;
      auto *m = Mapping::from_handle_id(handle_ids[i]);
      void *data = m->get_pointer();
      if (data == nullptr) continue;
      auto header = ObjectHeader::from(m);
      // An already localized object should not be double localized (yet)
      if (header->localized) continue;
      if (header->hotness < 0b111'111) header->hotness++;
    }

    if (dumps_recorded > 250) {
      dumps_recorded = 0;
      auto &ht = rt.handle_table;
      constexpr bool enable_scanning = true;

      int hot_cutoff = 2;

      uint64_t total_handles = 0;
      uint64_t total_hotness = 0;
      uint64_t scanned_hot = 0;
      uint64_t handles_seen_in_dump = 0;

      auto slabs = ht.get_slabs();
      for (auto *slab : slabs) {
        for (auto *allocated : slab->allocator) {
          auto *m = (alaska::Mapping *)allocated;
          if (m->get_pointer() == nullptr) continue;
          auto header = alaska::ObjectHeader::from(m->get_pointer());
          total_handles++;
          total_hotness += header->hotness;
          if (header->hotness != 0) {
            handles_seen_in_dump++;
          }

          if (header->hotness > hot_cutoff) {
            scanned_hot++;
          }
        }
      }



      alaska::printf("hot handles: %6zu, average:%4.2f, seen:%5.1f%%\n", scanned_hot,
          total_hotness / (float)total_handles,
          handles_seen_in_dump > 0 ? 100.0 * (float)scanned_hot / (float)handles_seen_in_dump : 0.0f);
    }

    // Push the buffer back to the queue of buffers
    struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    buf->next = buffers;
    buffers = buf;
  }
}  // namespace alaska
