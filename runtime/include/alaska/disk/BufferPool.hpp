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
#include <alaska/utils.h>
#include <ck/map.h>
#include <ck/option.h>
#include <ck/vec.h>

#include <alaska/RateCounter.hpp>

// Disk includes
#include <alaska/disk/Frame.hpp>
#include <alaska/disk/Disk.hpp>

namespace alaska::disk {

  static constexpr size_t buffer_pool_size = 16;  // Tweak me!


  class BufferPool final {
   public:
    BufferPool(const char *db_path, size_t size = 16);
    ~BufferPool();


    // No copy, no move, etc..
    BufferPool(BufferPool const &) = delete;
    void operator=(BufferPool const &t) = delete;
    BufferPool(BufferPool &&) = delete;



    // The main interface for a BufferPool
    FrameGuard getPage(uint64_t page_id);


    // Allocate a new Page
    FrameGuard newPage(bool useFreeList = true);
    // Free a Page, can be used again later
    void freePage(FrameGuard &&frame);

    // flush all dirty pages.
    void flush(void);

    // Dump statistics
    void dumpStats(void);


    template <typename T>
    T readValue(off_t byte_offset) {
      return *getPage(byte_offset / page_size).get<T>(byte_offset % page_size);
    }
    template <typename T>
    void writeValue(off_t byte_offset, const T &val) {
      *getPage(byte_offset / page_size).getMut<T>(byte_offset % page_size) = val;
    }

    ck::opt<uint64_t> getStructure(const char *name);
    void addStructure(const char *name, uint64_t root_page_id);

   private:
    struct StructureEntry {
      char name[16];
      uint64_t page_id;
    };

    struct Header {
      static constexpr uint32_t COOKIE_VALUE = 0xFEF1F0F3;
      static constexpr uint32_t MAX_STRUCTURE_COUNT = 32;
      uint32_t cookie;
      uint64_t next_free;

      uint64_t reserved[16];

      uint64_t num_structures;
      StructureEntry structures[MAX_STRUCTURE_COUNT];
    };

    static_assert(sizeof(Header) < page_size, "Header must be 512 bytes");
    Header header;




    // We implement a trival clock algorithm LRU approx.
    off_t clock_hand = 0;
    ck::vec<Frame> frames;

    ck::map<uint64_t, Frame *> frame_table;

    void *pool_memory = NULL;

    Disk disk;

    // Statistics
    alaska::RateCounter stat_accesses;
    alaska::RateCounter stat_hits;
    alaska::RateCounter stat_writebacks;
    alaska::RateCounter stat_disk_reads;
    alaska::RateCounter stat_disk_writes;


    inline void syncHeader(void) { writeValue<Header>(0, header); }

    // These exist just to track stats. They just forward to the disk
    bool readPage(uint64_t page_id, void *buf);
    bool writePage(uint64_t page_id, void *buf);
  };
}  // namespace alaska::disk