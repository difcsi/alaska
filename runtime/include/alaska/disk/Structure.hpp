/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */


#pragma once

#include <alaska/disk/BufferPool.hpp>

namespace alaska::disk {


  class Structure {
   public:
    Structure(BufferPool& pool, const char* name);


   protected:
    // Politely ask the structure to destroy itself (free all non-root pages)
    virtual void destroy() {}
    uint64_t root_page_id;
    BufferPool& pool;
    char name[16];

   private:
    inline uint64_t getRootPageID(BufferPool& pool, const char* name) {
      auto existing = pool.getStructure(name);
      if (existing) {
        this->root_page_id = existing.take();
      } else {
        this->root_page_id = pool.newPage().page_id();
        pool.addStructure(name, this->root_page_id);
      }
      return this->root_page_id;
    }
  };



  // Helper class to help with laying out structures in memory
  template <typename Header, typename Entry>
  struct HeaderAndEntries {
    Header header;
    static constexpr uint32_t NUM_ENTRIES =
        (alaska::disk::page_size - sizeof(Header)) / sizeof(Entry);
    Entry entries[NUM_ENTRIES];
  };

  // template <typename T>
  // class TempStructure {
  //   //
  // };

  // class Structure {
  //  public:
  //   Structure(BufferPool& pool, const char* name);
  //   // The root page of a structure is currently permanently stored on disk.
  //   // However, the other pages can be freed however the structure wants.

  //  protected:
  //   uint64_t root_page_id;
  //   BufferPool& pool;
  //   char name[16];
  // };
}  // namespace alaska::disk
