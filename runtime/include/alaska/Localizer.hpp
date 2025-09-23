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
#include "alaska/Runtime.hpp"
#include <ck/set.h>
#include <alaska/RateCounter.hpp>


namespace alaska {


  // fwd decl
  class ThreadCache;


  // This is a set of knobs that control the behavior of the localizer.
  // They are ordered roughly in terms of importance.


  struct LocalizerKnobs {
    float effort = 1.0;  // NOT USED
    // What is the maximum time allowed between dumps? (microseconds)
    long dump_interval_us = 100;  // 1000000;

    // How often do we act on dumps? (number of dumps)
    long localization_interval = 250;  // (50 * 1000) / dump_interval_us;

    // When localizing an object, how far do we recurse into the pointer graph of that object to
    // localize the objects it points to?
    long localization_depth = 0;

    // This value is used to determine which handles are hot and which aren't.
    long hotness_cutoff = 1;

    // If true, the localizer will attempt to relocalize handles that have
    // been localized before.
    bool relocalize = false;

    // If relocalizing is on, we multiply a handle's hotness by this value to
    // compute the effective hotness of the handle.
    float relocalize_ratio = 0.5;
  };

  class PIDController {
   public:
    PIDController(float kp, float ki, float kd)
        : kp_(kp)
        , ki_(ki)
        , kd_(kd)
        , prev_error_(0.0f)
        , integral_(0.0f) {}

    // Update the controller and return the control output
    float update(float target, float actual, float dt) {
      float error = target - actual;
      integral_ += error * dt;
      float derivative = (error - prev_error_) / dt;
      prev_error_ = error;

      return kp_ * error + ki_ * integral_ + kd_ * derivative;
    }

    // Optionally reset internal state
    void reset() {
      prev_error_ = 0.0f;
      integral_ = 0.0f;
    }

   private:
    float kp_, ki_, kd_;
    float prev_error_;
    float integral_;
  };

  class Localizer {
    alaska::ThreadCache &tc;
    size_t expected_count = 0;

    // Updated when parsing a dump image.
    alaska::RateCounter saturated_handle_counter;
    alaska::RateCounter invalid_handle_counter;
    alaska::RateCounter valid_handle_counter;
    alaska::RateCounter bytes_localized_counter;

    struct buffer {
      struct buffer *next;
    };
    struct buffer *buffers = nullptr;

    long dumps_recorded = 0;

    size_t saturated_bytes = 0;              // how many bytes are referenced by the handles in...
    ck::vec<handle_id_t> saturated_handles;  // ... the queue of hids which have saturated hotness.

    float compute_quality(handle_id_t *hids, size_t count, float *out_ratio = nullptr);



   public:
    // This is the set of handles that we gather in this dump
    constexpr static size_t MAX_DUMP_HANDLES = 4096;
    ck::vec<alaska::Mapping *> dump_handles;


    inline bool add_dump_handle(alaska::Mapping *m) {
      if (dump_handles.size() >= (int)MAX_DUMP_HANDLES) return false;  // cannot add more handles
      dump_handles.push(m);
      return true;
    }

    constexpr static size_t SEARCH_DEPTH = 16;
    bool discover_reachable_handles(alaska::Mapping *m, size_t depth = 0);

    size_t localized_objects = 0;
    float last_quality = 0;
    LocalizerKnobs knobs;
    Localizer(alaska::Configuration &config, alaska::ThreadCache &tc);

    // Get a hotness buffer that can fit `count` handle_ids.
    handle_id_t *get_hotness_buffer(size_t count);

    void localize_saturated_handles();


    struct ScanResult {
      long new_hot;    // how many new handles were considered hot?
      bool localized;  // did we perform localization?
    };

    // Give a hotness buffer back to the localizer, filled with `count` handle ids
    ScanResult feed_hotness_buffer(size_t count, handle_id_t *handle_ids);
  };
}  // namespace alaska
