/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#pragma once

#include <stdint.h>
#include <alaska/alaska.hpp>
#include <alaska/HandleTable.hpp> // for alaska::last_mapping

namespace alaska {



  struct ObjectHeader final {
    // The first 32 bits of the object header are the handle id. We use this
    // very often, so we want it to be fast as possible to access (without masks or anything)
    uint32_t handle_id;
    uint32_t size;  // The size of the object in bytes, not including the header.


    int32_t placement_badness;

    // And then theres some metadata
    union {
      struct {
        bool localized;  // A marker to quickly indicate if the object is localized
        bool marked;
      };
      uint32_t __metadata : 16;  // don't use this manually. just here to ensure space
    };

    inline size_t object_size(void) const { return this->size; }
    inline size_t real_object_size(void) const { return object_size() + sizeof(ObjectHeader); }

    void set_object_size(size_t size_bytes) { this->size = size_bytes; }
    void *data(void) { return (void *)((off_t)this + sizeof(ObjectHeader)); }

    inline void reset(void) {
      __metadata = 0;
      // placement_badness = 0;
    }
    alaska::Mapping *get_mapping(void) const { return alaska::Mapping::from_handle_id(handle_id); }
    // Passing null here means the object is not mapped.
    inline void set_mapping(const alaska::Mapping *m) {
      // clear metadata. This is important because we don't want to accidentally
      // have uninitialized metadata.
      reset();
      handle_id = m->handle_id();
    }


    inline void update_mapping(void) {
      alaska::Mapping *m = get_mapping();
      if (m == nullptr) return;
      m->set_pointer(this);
    }


    static ObjectHeader *from(alaska::Mapping &m) { return from(m.get_pointer()); }
    static ObjectHeader *from(alaska::Mapping *m) { return from(m->get_pointer()); }
    static ObjectHeader *from(void *ptr) {
      // return (ObjectHeader *)__builtin_assume_aligned((ObjectHeader *)((off_t)ptr -
      // sizeof(ObjectHeader)), sizeof(ObjectHeader));
      return WORD_ALIGNED((ObjectHeader *)((off_t)ptr - sizeof(ObjectHeader)));
    }


    void dump() {
      printf("<Object hid=%5zu size=%5zu ", (unsigned long)handle_id, object_size());
      printf("misc=%c", localized ? 'L' : ' ');
      // printf(" data=");
      // auto data = (uint8_t *)this->data();
      // for (size_t i = 0; i < object_size(); i++) {
      //   printf("%02x ", data[i]);
      // }
      printf(">");
    }



    template <typename Fn>
    void walk(Fn fn, size_t max_walk = 0) {
      // alaska::printf("Walking header %p\n", this);
      size_t size = object_size();
      if (max_walk == 0 || max_walk > size) max_walk = size;

      void **ptr = (void **)data();
      void **end = (void **)((uintptr_t)ptr + max_walk);
      uintptr_t offset = 0;
      for (void **ptr = (void **)data(); ptr < end; ptr++, offset++) {
        // Grab the pointer
        void *p = *ptr;

        // alaska::printf(" [%zu] ptr=%p -> %p\n", offset, ptr, p);

        void *pointee_data;
        alaska::Mapping *pointee_handle = nullptr;
        if (!alaska::check_mapping(p, pointee_handle, pointee_data)) continue;
        alaska::ObjectHeader *header = alaska::ObjectHeader::from(pointee_data);
        // alaska::printf("    handle=%p, data=%p, header=%p\n", pointee_handle, pointee_data, header);

        auto handle_offset = alaska::Mapping::offset_from_handle(p);
        if (handle_offset > header->object_size()) {
          continue;
        }

        // Check that the offset is valid.
        fn(pointee_handle, header);
      }
    }


  } __attribute__((packed));

  static_assert(sizeof(ObjectHeader) == 16, "ObjectHeader must be 8 bytes");
}  // namespace alaska
