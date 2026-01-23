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

#include <alaska/work/BarrierWorker.hpp>
#include <alaska/core/Runtime.hpp>

namespace alaska {


  BarrierWorker::BarrierWorker() {
    // Initialize the list
    m_list = LIST_HEAD_INIT(m_list);
  }


  BarrierWorker::~BarrierWorker() {
    // Destroy the list
    list_del(&m_list);
  }

  bool BarrierWorker::barrier_work_registered() {
    // There is not work registered if the list is empty
    // (ie: it is not a member of the runtime's barrier work list)
    return !list_empty(&m_list);
  }

  void BarrierWorker::register_barrier_work() {
    auto *rt = alaska::Runtime::get_ptr();
    if (rt == nullptr) {
      // If the runtime is not initialized, we cannot register barrier work
      return;
    }

    // TODO: LOCK!
    // Add this worker to the runtime's barrier work list
    list_add(&m_list, &rt->barrier_workers);
  }

  void BarrierWorker::deregister_barrier_work() {
    // Remove this worker from the runtime's barrier work list
    list_del(&m_list);
  }

}  // namespace alaska