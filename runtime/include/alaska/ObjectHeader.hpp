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

namespace alaska {



  struct ObjectHeader final {


    // The first 32 bits of the object header are the handle id. We use this
    // very often, so we want it to be fast as possible to access (without masks or anything)
    uint32_t handle_id;
    uint32_t size; // The size of the object in bytes, not including the header.


    int32_t placement_badness;

    // And then theres some metadata
    union {
      struct {
        bool localized;   // A marker to quickly indicate if the object is localized
        bool marked;
      };
      uint32_t __metadata : 16;  // don't use this manually. just here to ensure space
    };

    inline size_t object_size(void) const { return this->size; }
    inline size_t real_object_size(void) const { return object_size() + sizeof(ObjectHeader); }

    void set_object_size(size_t size_bytes) {
      this->size = size_bytes;
    }
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
      return (ObjectHeader *)((off_t)ptr - sizeof(ObjectHeader));
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


  } __attribute__((packed));

  static_assert(sizeof(ObjectHeader) == 16, "ObjectHeader must be 8 bytes");
}  // namespace alaska
