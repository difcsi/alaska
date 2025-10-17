
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
#include <limits.h>
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS
#include <math.h>

namespace alaska {

  // knobs
  constexpr float GOOD_QUALITY = 0.5;
  constexpr int64_t BADNESS_CUTOFF = 5;
  constexpr int64_t LOCALIZE_CUTOFF = 5;
  constexpr int64_t STEP = 1;
  constexpr int64_t LOCALIZATION_INTERVAL = 50;




  Localizer::Localizer(alaska::Configuration &config, alaska::ThreadCache &tc)
      : tc(tc) {
    if (getenv("HOT_CUTOFF") != NULL) {
      knobs.hotness_cutoff = atoi(getenv("HOT_CUTOFF"));
    }

    dump_handles.ensure_capacity(MAX_DUMP_HANDLES);
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


  static inline bool check_handle(void *p, alaska::Mapping *&m, void *&data) {
    m = alaska::Mapping::from_handle_safe(p);
    if (m == nullptr) return false;
    data = m->get_pointer();
    if (data == nullptr || m->is_free()) return false;
    return true;
  }

  static inline bool check_handle(handle_id_t hid, alaska::Mapping *&m, void *&data) {
    if (hid == 0) return false;
    m = alaska::Mapping::from_handle_id(hid);
    if (m == nullptr) return false;
    data = m->get_pointer();
    if (data == nullptr || m->is_free()) return false;
    return true;
  }



  float Localizer::compute_quality(handle_id_t *hids, size_t count, float *out_ratio) {
    /**
     * \sum object_size / num_pages  * 4096
     * \sum w_i object_size / num_pages  * 4096
     * w_i = 1 if object_quality > threshold else 0.5
     *
     * 1/(used_bytes) \sum w_i object_size
     */

    // First, compute the number of pages used by these objects.
    uintptr_t pages[count];
    lphashset_init(pages, count);

    size_t sum = 0;
    size_t used_bytes = 0;

    size_t good_objects = 0;
    size_t bad_objects = 0;

    for (size_t i = 0; i < count; i++) {
      alaska::Mapping *m = nullptr;
      void *data = nullptr;
      if (!check_handle(hids[i], m, data)) continue;
      auto *header = ObjectHeader::from(data);

      // the object size here is the total size the object occupies, including header.
      size_t object_size = header->real_object_size();

      if (header->placement_badness > BADNESS_CUTOFF) {
        bad_objects++;
      } else {
        good_objects++;
      }

      // If the object is currently bad enough that we consider it above a
      // certain threshold, we count it as half weight.
      sum += object_size >> (header->placement_badness > BADNESS_CUTOFF);

      for (size_t j = 0; j < object_size; j += 0x1000) {
        uintptr_t page = ((uintptr_t)data + j) >> 12;
        if (lphashset_insert(pages, count, page)) {
          used_bytes += 0x1000;
        }
      }
    }

    if (out_ratio) {
      *out_ratio = (float)good_objects / (float)(good_objects + bad_objects);
    }

    float quality = ((float)sum / (float)used_bytes);
    if (quality < 0) quality = 0;
    return quality;

    /*
    size_t bytes_needed = 0;
    size_t bytes_used = 0;
    uintptr_t pages[count];
    lphashset_init(pages, count);

    for (size_t i = 0; i < count; i++) {
      alaska::Mapping *m = nullptr;
      void *data = nullptr;
      if (!check_handle(hids[i], m, data)) continue;

      auto header = ObjectHeader::from(m);
      size_t object_size = header->object_size();
      // sizes.push(object_size);
      bytes_needed += object_size + sizeof(ObjectHeader);

      for (size_t j = 0; j < object_size; j += 0x1000) {
        uintptr_t page = ((uintptr_t)data + j) >> 12;
        if (lphashset_insert(pages, count, page)) {
          bytes_used += 0x1000;
        }
      }
    }

    float quality = ((float)bytes_needed / (float)bytes_used);
    if (quality < 0) quality = 0;

    return quality;
    */
  }




  bool Localizer::discover_reachable_handles(alaska::Mapping *m, size_t depth) {
    if (depth > SEARCH_DEPTH) return true;



    auto header = ObjectHeader::from(m);
    if (!this->tc.runtime.heap.contains(header)) {
      return true;
    }



    if (header->localized || header->marked) {
      // already localized, no need to discover more
      return true;
    }
    header->marked = true;


    if (!add_dump_handle(m)) return false;

    // for (size_t i = 0; i < depth; i++)
    //   alaska::printf("  ");
    // alaska::printf("%3d %p", depth, m->to_handle());
    // alaska::printf(" %p", m->get_pointer());
    // alaska::printf(" size=%zu", header->object_size());
    // alaska::printf("\n");



    auto data = header->data();

    size_t size = header->object_size();

    auto *ptr = (void **)header->data();
    size_t scan_size = 128;
    if (size < scan_size) scan_size = size;
    auto *end = (void **)((char *)ptr + scan_size);
    while (ptr < end) {
      auto *p = *ptr;
      if (p != nullptr) {
        auto *mp = alaska::Mapping::from_handle_safe(p);
        if (mp != nullptr) {
          if (this->tc.runtime.handle_table.valid_handle(mp)) {
            if (!discover_reachable_handles(mp, depth + 1)) {
              return false;
            }
          }
        }
      }
      ptr++;
    }

    return true;
  }



  Localizer::ScanResult Localizer::feed_hotness_buffer(size_t count, handle_id_t *handle_ids) {
    ScanResult res = {0};
    res.localized = false;
    dumps_recorded++;

    auto &rt = alaska::Runtime::get();


    // alaska::printf("YUKON_DUMP: %zu", alaska_timestamp());
    // for (size_t i = 0; i < count; i++) {
    //   if (handle_ids[i] == 0) continue;

    //   // check if it is valid or not.
    //   auto *m = Mapping::from_handle_id(handle_ids[i]);
    //   void *data = m->get_pointer();
    //   if (data == nullptr || m->is_free()) continue;
    //   auto header = ObjectHeader::from(m);
    //   alaska::printf(" %p:%zu", m->get_pointer(), header->object_size());
    // }
    // alaska::printf("\n");

    // // Push the buffer back to the queue of buffers
    // struct buffer *buf = reinterpret_cast<struct buffer *>(handle_ids);
    // buf->next = buffers;
    // buffers = buf;

    // return res;

#if 1

    // DumpQuality :: used_pages / needed_pages

    float ratio = 0.0f;
    float weighted_quality = compute_quality(handle_ids, count, &ratio);
    // alaska::printf("YUKON_QUALITY=%f, %f\n", weighted_quality, ratio);
    bool good_dump = weighted_quality > GOOD_QUALITY;

    // Reset the dump handles vector back to zero.
    dump_handles.reset_size();

    LocalityReport report;

    for (size_t i = 0; i < count; i++) {
      alaska::Mapping *m = nullptr;
      void *data = nullptr;
      if (!check_handle(handle_ids[i], m, data)) continue;


      //alaska::printf("GRADING: %p, %p\n", m, m->get_pointer());
      alaska::grade_locality(*m, 15, report);
      report.dump();
      // alaska::printf("    DONE GRADING: %p\n", m);


      // tc.localize(m, 4);  // Localize to a depth of four.


      // if (!discover_reachable_handles(m)) {
      //   break;
      // }



      // auto header = ObjectHeader::from(m);
      // header->marked = true;
      continue;
#if 0
      if (header->localized) continue;

      // If the object is already determined to need localization, skip it.
      if (header->placement_badness > LOCALIZE_CUTOFF) {
        continue;
      }


      bool was_good_object = header->placement_badness <= BADNESS_CUTOFF;

      if (good_dump) {
        if (was_good_object) {
          // ... nothing.
        } else {
          // bad object.
          header->placement_badness += STEP;
        }
      } else {
        // bad dump.
        if (was_good_object) {
          // good object.
          header->placement_badness += STEP * ratio;
        } else {
          // bad object.
          header->placement_badness += STEP;
        }
      }

      header->marked = true;


      if (header->placement_badness > LOCALIZE_CUTOFF) {
        saturated_handles.push(handle_ids[i]);
      }
#endif
    }

    report.dump();

    // // alaska::printf("Discovered %zu handles\n", dump_handles.size());
    // for (auto *m : dump_handles) {
    //   if (m == nullptr) continue;
    //   auto header = ObjectHeader::from(m);
    //   // header->placement_badness += STEP;
    //   // if (header->placement_badness > LOCALIZE_CUTOFF) {
    //   saturated_handles.push(m->handle_id());
    //   // saturated_bytes += header->real_object_size();
    //   // }
    // }




    /////////////

#if 0
    bool should_localize = false;
    // if (dumps_recorded > knobs.localization_interval) should_localize = true;
    if (saturated_handles.size() > 256) should_localize = true;
    // should_localize = false;



    if (should_localize) {
      // alaska::printf("Localizing! %zu in queue\n", saturated_handles.size());
      uint64_t scanned_hot = 0;
      dumps_recorded = 0;
      res.localized = true;

      localize_saturated_handles();

      // rt.heap.dump(stdout);
      // rt.heap.compact_locality_pages();
      // rt.heap.compact_sizedpages();

      saturated_bytes = 0;
      saturated_handles.clear();
    }
#endif


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
      float quality_before = compute_quality(saturated_handles.data(), saturated_handles.size());
      // iterate over our queue of saturated handles from start to end, and localize those which are
      // valid. NOTE: this does not clear the queue, just localizes them.

      long num_localized = 0;

      for (auto &handle : saturated_handles) {
        if (handle == 0) continue;
        auto *m = Mapping::from_handle_id(handle);
        void *data = m->get_pointer();
        if (data == nullptr || m->is_free()) continue;
        auto header = ObjectHeader::from(m);
        if (!header->marked) continue;
        auto headerBefore = ObjectHeader::from(m);
        size_t objectSize = headerBefore->object_size();
        // if the handle is valid, localize it
        num_localized += this->tc.localize(m, 0);
        bytes_localized_counter.track(objectSize);
        // printf("localized %p %zu \n", m, object_size);
        localized_objects += objectSize;
      }

      alaska::printf("Localized %ld objects\n", num_localized);
      // alaska::printf("LOCALIZING ");
      // for (auto &handle : saturated_handles) {
      //   if (handle == 0) continue;
      //   alaska::printf("%lu ", handle);
      // }
      // alaska::printf("\n");

      float quality_after = compute_quality(saturated_handles.data(), saturated_handles.size());

      // alaska::printf("Localized %zu handles, quality before: %f, after: %f\n",
      //     saturated_handles.size(), quality_before, quality_after);
    });
    if (!did_localize) {
      printf("Failed to localize\n");
    }
  }
}  // namespace alaska
