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


  class StructureImpl {
   public:
    StructureImpl(BufferPool& pool, uint64_t root_page_id);


   private:
    uint64_t root_page_id;
    BufferPool& pool;
  };


  template <typename T>
  class NamedStructure : public T {
   public:
    NamedStructure(BufferPool& pool, const char* name)
        : pool(pool)
        , T(pool, getRootPageID(pool, name)) {
      //
    }


   private:
    BufferPool& pool;
    uint64_t root_page_id;
    char name[16];

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




  class BPlusTree : public StructureImpl {
   public:
    BPlusTree(BufferPool& pool, uint64_t root_page_id)
        : StructureImpl(pool, root_page_id) {
      printf("BPlusTree created with rootid = %zu\n", root_page_id);
    }
    //
  };


  using NamedBPlusTree = NamedStructure<BPlusTree>;

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
