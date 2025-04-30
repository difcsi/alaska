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


  // This is a set of knobs that control the behavior of the localizer.
  // They are ordered roughly in terms of importance.


  struct LocalizerKnobs {
    // What is the maximum time allowed between dumps? (microseconds)
    long dump_interval_us = 50;

    // How often do we act on dumps? (number of dumps)
    long localization_interval = 64;  // (50 * 1000) / dump_interval_us;

    // When localizing an object, how far do we recurse into the pointer graph of that object to
    // localize the objects it points to?
    long localization_depth = 4;

    // This value is used to determine which handles are hot and which aren't.
    // A value of N here means that if a handle has been seen in >N dumps, it is hot.
    long hotness_cutoff = 24;

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

    struct buffer {
      struct buffer *next;
    };
    struct buffer *buffers = nullptr;

    long dumps_recorded = 0;

    ck::map<handle_id_t, ck::map<handle_id_t, uint64_t>> dump_connectivity;


   public:
    LocalizerKnobs knobs;
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
