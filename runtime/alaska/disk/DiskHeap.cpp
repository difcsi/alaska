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

#include <stdlib.h>
#include <string.h>
#include <alaska/disk/DiskHeap.hpp>
#include "alaska/heaps/Heap.hpp"
#include "ck/vec.h"
#include <alaska/util/utils.h>
#include <errno.h>
#include <sys/stat.h>



#define NON_ZERO_ASSERT(stmt)                              \
  ({                                                       \
    auto res = stmt;                                       \
    ALASKA_ASSERT(stmt != 0, "returned non zero: " #stmt); \
    res;                                                   \
  })

namespace alaska {

  namespace diskheap {


    DiskHeap::DiskHeap(const char *db_path)
        : pool(db_path) {
      // Load the header off disk.
      this->header = readValue<Header>(0);

      if (header.cookie != Header::MAGIC_COOKIE) {
        printf("header is not valid. resetting\n");
        memset(&header, 0, sizeof(Header));

        header.cookie = Header::MAGIC_COOKIE;
        header.numSuperPages = 0;
        syncHeader();
      } else {
        printf("header is valid!\n");
      }

      if (header.handleMapRootPage == 0) {
        header.handleMapRootPage = allocateDataPage();
        syncHeader();
      }


      ck::map<uint64_t, uint64_t> vals;

      auto count = 4096 * 32;
      for (int i = 0; i < count; i++) {
        uint32_t hid = rand() % count;
        vals[hid] = rand();
        handleMapSet(hid, vals[hid]);
      }

      for (auto &[k, v] : vals) {
        auto r = handleMapGet(k);
        ALASKA_ASSERT(r.has_value(), "walk must return a value");
        ALASKA_ASSERT(r.take() == v, "value must be correct");
      }
    }

    DiskHeap::~DiskHeap(void) {}

    void DiskHeap::dump(void) {
      //
    }


    void DiskHeap::syncHeader(void) { this->writeValue<Header>(0, header); }

    SuperPageID DiskHeap::allocateSuperPage(void) {
      printf("allocate new super page\n");
      SuperPageID new_id = header.numSuperPages;

      auto page = getSuperPage(new_id);
      auto *sp = page.get_write<SuperPageOverlay>(0);

      sp->initialize();

      header.numSuperPages++;
      syncHeader();

      return new_id;
    }

    PageID DiskHeap::allocateDataPage() {
      while (1) {
        for (SuperPageID id = 0; id < header.numSuperPages; id++) {
          auto sp = getSuperPage(id);
          auto res = sp.get_write<SuperPageOverlay>()->allocate(PageType::DataPage, id);
          if (res.has_value()) {
            auto val = res.take() + 1;
            return val;
          }
        }

        allocateSuperPage();
      }
      return 0;
    }


#define PXMASK ((1llu << bits_per_hm_level) - 1)  // N bits
#define PXSHIFT(level) (bits_per_hm_level * (level))
#define PX(level, hid) ((((uint64_t)(hid)) >> PXSHIFT(level)) & PXMASK)

    void DiskHeap::handleMapSet(uint64_t hid, uint64_t value) {
      PageID nodePage = header.handleMapRootPage;
      ALASKA_ASSERT(nodePage != 0, "Invalid root for handle map");
      for (int i = (int)hm_levels - 1; i >= 0; i--) {
        int index = PX(i, hid);
        auto page = pool.getPage(nodePage);
        if (i == 0) {
          auto n = page.get_write<HandleMapNodeOverlay>();
          n->entries[index] = value;
          return;
        }

        PageID next = page.get<HandleMapNodeOverlay>()->entries[index];
        if (next == 0) {
          // Allocate next level
          next = allocateDataPage();
          page.get_write<HandleMapNodeOverlay>()->entries[index] = next;
        }

        nodePage = next;
      }
      abort();
    }

    ck::opt<uint64_t> DiskHeap::handleMapGet(uint64_t hid) {
      PageID nodePage = header.handleMapRootPage;
      ALASKA_ASSERT(nodePage != 0, "Invalid root for handle map");
      for (int i = (int)hm_levels - 1; i >= 0; i--) {
        int index = PX(i, hid);
        auto page = pool.getPage(nodePage);
        if (i == 0) {
          auto n = page.get_write<HandleMapNodeOverlay>();
          return n->entries[index];
        }

        PageID next = page.get<HandleMapNodeOverlay>()->entries[index];
        if (next == 0) return None;
        nodePage = next;
      }
      return None;
    }


    void SuperPageOverlay::initialize(void) {
      printf("Initialize super page.\n");

      this->header.next_bump = 0;  // the 0th data page is the next one to allocate
      for (size_t i = 0; i < data_pages_per_superpage; i++) {
        info[i].type = PageType::UnallocatedPage;
      }
    }

    ck::opt<uint64_t> SuperPageOverlay::allocate(PageType t, SuperPageID my_id) {
      if (header.next_bump == data_pages_per_superpage) return None;
      auto ind = header.next_bump++;
      info[ind].type = t;
      return my_id + ind + 1;
    }


    // BufferPool


    BufferPool::BufferPool(const char *db_path)
        : pf(db_path, page_size) {
      pool_memory = mmap_alloc(page_size * buffer_pool_size);
      for (size_t i = 0; i < buffer_pool_size; i++) {
        auto &f = frames[i];
        f.clear();
        f.memory = (void *)((uint8_t *)pool_memory + (page_size * i));
      }
    }

    BufferPool::~BufferPool(void) {
      flush();
      mmap_free(pool_memory, page_size * buffer_pool_size);

      dumpStats();
    }

    void BufferPool::flush(void) {
      for (size_t i = 0; i < buffer_pool_size; i++) {
        auto &f = frames[i];
        if (f.valid && f.dirty) {
          // RACE! (someone could be actively writing)
          f.dirty = false;
          writePage(f.page_id, f.memory);
        }
      }
    }

    FrameRef BufferPool::getPage(uint64_t page_id) {
      // validate();
      stat_accesses++;

      // BAD:


      auto it = frame_table.find(page_id);
      if (it != frame_table.end()) {
        stat_hits++;
        auto *f = it->value;
        ALASKA_ASSERT(f->page_id == page_id, "frame table invalid");
        f->ref = true;
        return f;
      }


      int retry_loop_count = 3;
      // LRU Evict
      for (size_t i = 0; i < buffer_pool_size * retry_loop_count; i++) {
        off_t loc = clock_hand++;
        if (clock_hand >= (off_t)buffer_pool_size) clock_hand = 0;
        auto &f = frames[loc];

        // If someone is using it, we can't use it.
        if (f.get_refcount() != 0) {
          continue;
        }

        if (f.ref) {
          // if the frame has been referenced, clear that fact and continue.
          f.ref = false;
          continue;
        }

        // If the frame is valid (has a page), we might need to flush it.
        if (f.valid && f.dirty) {
          // evict
          stat_writebacks++;

          // printf("Evict %zu for %zu\n", f.page_id, page_id);
          writePage(f.page_id, f.memory);
          f.dirty = false;
        }

        // Remove the old frame mapping
        frame_table.remove(f.page_id);
        f.reset(page_id);
        // Add the new mapping
        frame_table.set(page_id, &f);
        f.ref = true;  // the page was accessed

        readPage(f.page_id, f.memory);

        return &f;
      }
      printf("found nothing.\n");

      return nullptr;
    }

    void BufferPool::validate(void) {
      bool failed = false;
      for (size_t i = 0; i < buffer_pool_size; i++) {
        auto &f1 = frames[i];
        if (!f1.valid && f1.dirty) {
          printf("VALIDATE FAILED: page_id %zu is not valid, but dirty\n", f1.page_id);
          failed = true;
        }

        if (!f1.valid && f1.ref) {
          printf("VALIDATE FAILED: page_id %zu is not valid, but ref'd\n", f1.page_id);
          failed = true;
        }

        for (size_t j = 0; j < buffer_pool_size; j++) {
          if (j == i) continue;
          auto &f2 = frames[j];

          if (f1.memory == f2.memory) {
            printf("VALIDATE FAILED: two pages share memory: (%zu, %zu)\n", i, j);
            failed = true;
          }

          if (f1.valid && f2.valid) {
            if (f1.page_id == f2.page_id) {
              printf("VALIDATE FAILED: page_id %zu is mapped twice: (%zu, %zu)\n", f1.page_id, i,
                     j);
              failed = true;
            }
          }
        }
      }
      if (failed) {
        printf("VALIDATION FAILED\n");
        dump();
        exit(-1);
      }
    }

    void BufferPool::dump(void) {
      printf("BufferPool: (%f%% hr) ", 100.0 * this->stat_hits.read() / this->stat_accesses.read());
      for (size_t i = 0; i < buffer_pool_size; i++) {
        auto &f = frames[i];
        if (i != 0) printf(" ");


        if (f.valid) {
          printf(" %4zu", f.page_id);
        } else {
          printf("     ");
        }

        printf("%c", f.valid ? 'v' : ' ');
        printf("%c", f.ref ? 'r' : ' ');
        printf("%c", f.get_refcount() > 0 ? 'P' : ' ');
        printf("%c", f.dirty ? 'D' : ' ');
      }

      printf("\n");
    }


    void BufferPool::dumpStats(void) {
      printf("Buffer Pool Stats:\n");

      printf("  accesses:    %12zu (%.1f/s)\n", stat_accesses.read(), stat_accesses.digest());
      printf("  hits:        %12zu (%.1f/s)\n", stat_hits.read(), stat_hits.digest());
      // printf("  writebacks:  %12zu (%.1f/s)\n", stat_writebacks.read(),
      // stat_writebacks.digest());

      printf("  disk reads:  %12zu (%.1f/s)\n", stat_disk_reads.read(), stat_disk_reads.digest());
      printf("  disk writes: %12zu (%.1f/s)\n", stat_disk_writes.read(), stat_disk_writes.digest());

      printf("  -----\n");
      printf("  hitrate:     %12.4f%%\n", 100.0 * stat_hits.read() / stat_accesses.read());
      printf("\n");
    }


    size_t BufferPool::numPagesInFile() { return pf.fileSize() / page_size; }


    void hexdump(const void *v, size_t len) {
      const uint8_t *p = (const uint8_t *)v;
      for (size_t i = 0; i < len; i++) {
        if (i != 0) printf(" ");
        printf("%02x", p[i]);
      }
      printf("\n");
    }

    bool BufferPool::readPage(uint64_t page_id, void *buf) {
      // printf("read page %zu\n", page_id);
      stat_disk_reads.track();
      return pf.readPage(page_id, buf);
    }



    bool BufferPool::writePage(uint64_t page_id, void *buf) {
      // printf("write page %zu\n", page_id);
      stat_disk_writes.track();
      return pf.writePage(page_id, buf);
    }




    // PageFile

    PageFile::PageFile(const char *filename, size_t page_size)
        : m_page_size(page_size) {
      m_file = open(filename, O_CREAT | O_RDWR, 0644);
      struct stat st;
      if (fstat(m_file, &st) != 0) {
        printf("could not stat!\n");
        abort();
      }
      last_known_size = st.st_size;
    }

    PageFile::~PageFile() {
      if (m_file) {
        close(m_file);
      }
    }

    bool PageFile::readPage(uint64_t page_id, void *buf) {
      // if (!m_file || !buf) return false;
      off_t offset = page_id * m_page_size;
      if (!ensureFileSize(offset + m_page_size)) return false;
      if (lseek(m_file, offset, SEEK_SET) != offset) {
        return false;
      }
      bool success = read(m_file, buf, m_page_size) == (ssize_t)m_page_size;
      // hexdump(buf, 16);
      return success;
    }

    bool PageFile::writePage(uint64_t page_id, const void *buf) {
      off_t offset = page_id * m_page_size;
      if (!ensureFileSize(offset + m_page_size)) return false;
      if (lseek(m_file, offset, SEEK_SET) != offset) return false;
      return write(m_file, buf, m_page_size) == (ssize_t)m_page_size;
    }

    bool PageFile::ensureFileSize(size_t min_size) {
      if (last_known_size < min_size) {
        last_known_size = min_size;
        return ftruncate(m_file, min_size) == 0;
      }
      return true;
    }

  }  // namespace diskheap
}  // namespace alaska
