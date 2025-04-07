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

#include <alaska/alaska.hpp>
#include <alaska/Configuration.hpp>
#include <ck/set.h>

namespace alaska {


  // fwd decl
  class ThreadCache;


  class Localizer {
    alaska::ThreadCache &tc;
    size_t expected_count = 0;
    long hotness_cutoff = 24;


    struct buffer {
      struct buffer *next;
    };
    struct buffer *buffers = nullptr;

    uint64_t dumps_recorded = 0;

    ck::map<handle_id_t, ck::map<handle_id_t, uint64_t>> dump_connectivity;


   public:
    Localizer(alaska::Configuration &config, alaska::ThreadCache &tc);

    // Get a hotness buffer that can fit `count` handle_ids.
    handle_id_t *get_hotness_buffer(size_t count);


    struct ScanResult {
      long new_hot;  // how many new handles were considered hot?
    };

    // Give a hotness buffer back to the localizer, filled with `count` handle ids
    ScanResult feed_hotness_buffer(size_t count, handle_id_t *handle_ids);
  };
}  // namespace alaska
