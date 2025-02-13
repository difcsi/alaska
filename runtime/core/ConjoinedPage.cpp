/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 */
/** This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#include <alaska/ConjoinedPage.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/Logger.hpp>
#include <alaska/SizedAllocator.hpp>
#include <string.h>
#include <ck/template_lib.h>


namespace alaska {

  ConjoinedPage::~ConjoinedPage(void) {
    //
  }

  void* ConjoinedPage::allocate_handle(const AllocationRequest& req) {
    // TODO:
    return nullptr;
  }

  bool ConjoinedPage::release_local(const Mapping& m, void* ptr) {
    // TODO:
    return false;
  }

  bool ConjoinedPage::release_remote(const Mapping& m, void* ptr) {
    // TODO:
    return false;
  }

  size_t ConjoinedPage::size_of(void* ptr) {
    // TODO:
    return 0;
  }

}  // namespace alaska
