/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */

#pragma once


#include <stdint.h>
#include <stdlib.h>
#include <alaska/util/utils.h>


namespace alaska::disk {

  static constexpr size_t page_order = 12;
  static constexpr size_t page_size = 1LU << page_order;


  // A disk is a simple wrapper aruond a file descriptor, exposing
  // reading/writing of pages
  class Disk {
   public:
    Disk(const char* path);
    ~Disk();

    // Read a page
    bool readPage(uint64_t page, void* buf);
    // Write a page
    bool writePage(uint64_t page, const void* buf);
    // How many pages are there?
    inline size_t pageCount(void) { return last_known_size / page_size; }

    // Ensure the disk is at least a specific size
    bool ensureFileSize(size_t min_size);

   private:
    int m_file;
    size_t last_known_size = 0;
  };


}  // namespace alaska::disk