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

#include <unistd.h>
#include <alaska/heaps/HeapPage.hpp>

namespace alaska {

  static constexpr uint64_t alignment = 16;

  static constexpr uint64_t small_word_size = alignment;
  static constexpr uint64_t large_word_size = alignment * 4;


  static constexpr uint64_t max_small_size = 1024;  // small objects are < 1024 bytes
  static constexpr uint64_t max_large_size = alaska::huge_object_thresh;


  static constexpr uint64_t num_small_classes = max_small_size / small_word_size;
  static constexpr uint64_t num_large_classes = (max_large_size - max_small_size) / large_word_size;

  static constexpr uint64_t num_size_classes = num_small_classes + num_large_classes;



  using size_class_t = uint64_t;

  inline size_class_t size_to_class_small(size_t sz) {
    return (sz + small_word_size - 1) / small_word_size;
  }

  inline size_class_t size_to_class_large(size_t sz) {
    size_t large_offset = sz - max_small_size;
    return ((large_offset + large_word_size - 1) / large_word_size) + num_small_classes;
  }

  inline size_class_t size_to_class(size_t size) {
    if (size < max_small_size) return size_to_class_small(size);
    return size_to_class_large(size);
  }



  inline size_t class_to_size(size_class_t cls) {
    if (cls < num_small_classes) {
      return cls * small_word_size;
    }
    return (cls - num_small_classes) * large_word_size + max_small_size;
  }


  inline bool should_be_huge_object(size_t size) {
    //
    return size >= max_large_size;
  }


  inline size_t round_up_size(size_t size) {
    if (should_be_huge_object(size)) return size;
    return class_to_size(size_to_class(size));
  }

}  // namespace alaska
