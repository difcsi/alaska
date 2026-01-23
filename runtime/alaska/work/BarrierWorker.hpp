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



#include <alaska/alaska.hpp>
#include "alaska/util/list_head.h"
namespace alaska {

  // The return value from the `barrier_work()` function
  enum BarrierWorkResult {
    BARRIER_WORK_REMAIN,
    BARRIER_WORK_DONE,
  };

  class BarrierWorker : public alaska::InternalHeapAllocated {
   public:
    BarrierWorker();
    virtual ~BarrierWorker();

    void register_barrier_work();
    void deregister_barrier_work();

    bool barrier_work_registered();

    // The barrier_work function is called by the barrier
    // when the worker is ready to do some work. The worker
    // should return BARRIER_WORK_DONE when it is finished
    // with its work, and BARRIER_WORK_REMAIN when it still
    // has work to do.
    virtual BarrierWorkResult barrier_work(void) = 0;

   protected:
    friend struct Runtime;
    struct list_head m_list;
  };

}  // namespace alaska
