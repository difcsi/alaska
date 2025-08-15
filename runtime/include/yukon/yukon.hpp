/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2023, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2023, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#pragma once


#include <alaska/ThreadCache.hpp>

/* Used to chat w/ the kernel module to deliver timer interrupts for dumping */
#include <linux/ioctl.h>
struct yukon_schedule_arg {
  unsigned long handler_address;  // Address of the handler function
  unsigned long delay;            // Delay in microseconds before scheduling
};

#define YUKON_IOCTL_MAGIC 'y'
#define YUKON_IOCTL_SCHEDULE _IOR(YUKON_IOCTL_MAGIC, 0, struct yukon_schedule_arg *)
#define YUKON_IOCTL_RETURN _IO(YUKON_IOCTL_MAGIC, 1)



namespace yukon {

  constexpr int csr_htbase = 0xc2;
  constexpr int csr_htdump = 0xc3;
  constexpr int csr_htinval = 0xc4;

  // Set the handle table base register in hardware to enable handle translation
  void set_handle_table_base(void *base);
  // Trigger an HTLB dump
  void dump_htlb(alaska::ThreadCache *tc);


  void init(void);


  void dump_alarm_handler(int sig);


}  // namespace yukon
