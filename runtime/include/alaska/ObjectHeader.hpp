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
    // we track the size of the object in the header by counting the number of "blocks"
    // that the object takes up. Really, this is the size over 8
    // This is just to save bits here :)
    uint16_t blocks;

    // And then theres some metadata
    union {
      struct {
        bool localized : 1;   // A marker to quickly indicate if the object is localized
        uint8_t hotness : 6;  // saturating counter for how many times an object has been in a dump
      };
      uint16_t __metadata : 16;  // don't use this manually. just here to ensure space
    };


    uint32_t hit_count;
    uint32_t __reserved;

    inline size_t object_size(void) const { return this->blocks * 8; }
    inline size_t real_object_size(void) const { return object_size() + sizeof(ObjectHeader); }

    void set_object_size(size_t size_bytes) {
      this->blocks = round_up(size_bytes, 8) / 8;
      ALASKA_SANITY(this->blocks != 0, "object size must be greater than 0");
    }
    void *data(void) { return (void *)((off_t)this + sizeof(ObjectHeader)); }

    inline void reset(void) {
      __metadata = 0;
      hit_count = 0;
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
