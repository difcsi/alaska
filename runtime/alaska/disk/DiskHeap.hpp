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

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <ck/lock.h>
#include <ck/map.h>
#include <alaska/util/utils.h>
#include <fcntl.h>
#include <alaska/config.h>
#include <alaska/util/RateCounter.hpp>
#include <ck/option.h>


namespace alaska {

  namespace diskheap {


    // the disk heap is organized as a set of "super pages" which contain pages within.
    // The types of the pages are dictated by an "info page" at the start of each super page which
    // is simply an array of `PageInfo`s which indicate the type of each page along with any other
    // info that is needed.
    // For simplicity, the db file's size is rounded to the size of a super page (storage is cheap)

    enum class PageType : uint32_t {
      UnallocatedPage,
      InfoPage,
      DataPage,
    };

    struct SuperPageHeader final {
      uint32_t next_bump;
    };

    struct PageInfo final {
      PageType type;
    };

    static_assert(sizeof(SuperPageHeader) == sizeof(PageInfo));

    static constexpr size_t page_order = 12;
    static constexpr size_t page_size = 1LU << page_order;
    static constexpr size_t pages_per_superpage = page_size / sizeof(PageInfo);
    static constexpr size_t buffer_pool_size = 16;  // Tweak me!



    // a Frame is an in-memory cache of a Page. They are managed by the BufferPool
    class Frame final {
     public:
      // If the frame is referenced since the last time the clock looked.
      bool ref = false;
      bool dirty = false;
      bool valid = false;

      void *memory;  // Backing data
      uint64_t page_id = 0;
      int refcount = 0;


      inline void clear(void) {
        ref = false;
        dirty = false;
        valid = false;
        page_id = 0;
      }

      inline void reset(uint64_t page_id) {
        this->ref = false;
        this->valid = true;
        this->dirty = false;
        this->page_id = page_id;
        this->refcount = 0;
      }

     protected:
      friend class FrameRef;
      friend class BufferPool;
      int get_refcount(void) { return atomic_get(this->refcount); }
      void inc_refcount(void) { atomic_inc(this->refcount, 1); }
      void dec_refcount(void) { atomic_dec(this->refcount, 1); }
    };




    class FrameRef final {
     protected:
      Frame *frame = NULL;

      void set(Frame *new_frame) {
        auto old_frame = this->frame;
        if (new_frame) {
          new_frame->inc_refcount();
        }
        this->frame = new_frame;
        if (old_frame) {
          old_frame->dec_refcount();
        }
      }

     public:
      inline FrameRef(Frame *frame) { set(frame); }
      inline FrameRef(FrameRef &other) { set(other.frame); }
      inline FrameRef(FrameRef &&other) {
        set(other.frame);
        other.set(nullptr);
      }
      inline FrameRef &operator=(const FrameRef &other) {
        set(other.frame);
        return *this;
      }

      inline ~FrameRef(void) { set(nullptr); }

      template <typename T>
      const T *get(off_t byte_offset = 0) {
        frame->ref = true;
        return (const T *)((uint8_t *)frame->memory + byte_offset);
      }
      template <typename T>
      T *get_write(off_t byte_offset = 0) {
        frame->ref = true;
        frame->dirty = true;
        return (T *)((uint8_t *)frame->memory + byte_offset);
      }
    };


    class PageFile {
     public:
      PageFile(const char *filename, size_t page_size);
      ~PageFile();

      bool readPage(uint64_t page_id, void *buf);
      bool writePage(uint64_t page_id, const void *buf);
      bool ensureFileSize(size_t min_size);

      inline size_t fileSize(void) { return last_known_size; }

     private:
      int m_file;
      size_t m_page_size;
      size_t last_known_size = 0;
    };


    class BufferPool {
     private:
      // Implement a simple 'clock' algorithm for lru
      // TODO: use something like LRU-k (a simple chain)
      Frame frames[buffer_pool_size];

      // Quickly map from page_id to frame, if they are cached
      ck::map<uint64_t, Frame *> frame_table;
      off_t clock_hand = 0;

      void *pool_memory = NULL;


      // statistic tracking.
      alaska::RateCounter stat_accesses;
      alaska::RateCounter stat_hits;
      alaska::RateCounter stat_writebacks;
      // uint64_t stat_disk_reads = 0;
      // uint64_t stat_disk_writes = 0;

      alaska::RateCounter stat_disk_reads;
      alaska::RateCounter stat_disk_writes;

      PageFile pf;

     public:
      BufferPool(const char *db_path);
      ~BufferPool();

      BufferPool(BufferPool const &) = delete;
      void operator=(BufferPool const &t) = delete;
      BufferPool(BufferPool &&) = delete;

      FrameRef getPage(uint64_t page_id);

      void dump(void);
      void dumpStats(void);

      // flush outstanding writes.
      void flush(void);
      void validate();

      size_t numPagesInFile(void);

     private:
      bool readPage(uint64_t page_id, void *buf);
      bool writePage(uint64_t page_id, void *buf);
    };


    using SuperPageID = uint64_t;
    using PageID = uint64_t;

    struct SuperPageOverlay {
      static constexpr size_t data_pages_per_superpage = pages_per_superpage - 1;

      SuperPageHeader header;
      PageInfo info[data_pages_per_superpage];
      void initialize();
      // Allocate a page in this super page, and return the absolute page index
      ck::opt<uint64_t> allocate(PageType t, SuperPageID my_id);
    };
    static_assert(sizeof(SuperPageOverlay) == page_size);


    static constexpr uint64_t hm_total_bits = 64 - ALASKA_SIZE_BITS;
    static constexpr uint64_t entries_per_handle_map_level = page_size / sizeof(PageID);
    static constexpr uint64_t bits_per_hm_level = page_order - 3;
    static constexpr uint64_t hm_levels = hm_total_bits / bits_per_hm_level;
    static_assert(sizeof(PageID) == 8);


    struct HandleMapNodeOverlay {
      PageID entries[entries_per_handle_map_level];
    };
    static_assert(sizeof(HandleMapNodeOverlay) == page_size);



    // A DiskHeap implements a simple heap on disk. It implements
    // a pretty simple buffer pool to limit memory usage, as we are
    // mostly worried about "swapping" handles out to disk when they
    // are extremely cold.
    //
    // This DiskHeap structure really only orchestrates access to the
    // buffer pool, which is managed by the pool structure.
    class DiskHeap final {
     public:
      DiskHeap(const char *db_path);
      DiskHeap(DiskHeap const &) = delete;
      DiskHeap(DiskHeap &&) = delete;
      void operator=(DiskHeap const &t) = delete;
      ~DiskHeap(void);


      void dump(void);


      void handleMapSet(uint64_t hid, uint64_t value);
      ck::opt<uint64_t> handleMapGet(uint64_t hid);

     private:
      struct Header {
        static constexpr uint32_t MAGIC_COOKIE = 0xDEADBEEF;

        uint32_t cookie;
        uint64_t numSuperPages;

        // the root page of the handle map radix tree (it's organized like a page table)
        PageID handleMapRootPage;
      };

      // a copy of the on-disk header
      Header header;
      // The BufferPool manager
      BufferPool pool;


      template <typename T>
      T readValue(off_t byte_offset) {
        return *pool.getPage(byte_offset / page_size).get<T>(byte_offset % page_size);
      }
      template <typename T>
      void writeValue(off_t byte_offset, const T &val) {
        *pool.getPage(byte_offset / page_size).get_write<T>(byte_offset % page_size) = val;
      }

      void syncHeader(void);
      FrameRef getSuperPage(SuperPageID super_page_id) {
        return pool.getPage(1 + (super_page_id * pages_per_superpage * page_size));
      }

      // Bump allocate a new super page and return the super page id.
      SuperPageID allocateSuperPage(void);
      PageID allocateDataPage();
    };
  }  // namespace diskheap


}  // namespace alaska
