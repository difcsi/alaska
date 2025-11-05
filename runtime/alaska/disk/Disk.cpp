/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */


#include <alaska/disk/Disk.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

namespace alaska::disk {

  Disk::Disk(const char *filename) {
    m_file = open(filename, O_CREAT | O_RDWR, 0644);
    struct stat st;
    if (fstat(m_file, &st) != 0) {
      printf("could not stat!\n");
      abort();
    }
    last_known_size = st.st_size;
  }

  Disk::~Disk() {
    if (m_file) {
      close(m_file);
    }
  }

  bool Disk::readPage(uint64_t page_id, void *buf) {
    off_t offset = page_id * page_size;
    if (!ensureFileSize(offset + page_size)) return false;
    if (lseek(m_file, offset, SEEK_SET) != offset) {
      return false;
    }
    bool success = read(m_file, buf, page_size) == (ssize_t)page_size;
    return success;
  }

  bool Disk::writePage(uint64_t page_id, const void *buf) {
    off_t offset = page_id * page_size;
    if (!ensureFileSize(offset + page_size)) return false;
    if (lseek(m_file, offset, SEEK_SET) != offset) return false;
    return write(m_file, buf, page_size) == (ssize_t)page_size;
  }

  bool Disk::ensureFileSize(size_t min_size) {
    if (last_known_size < min_size) {
      last_known_size = min_size;
      return ftruncate(m_file, min_size) == 0;
    }
    return true;
  }


}  // namespace alaska::disk