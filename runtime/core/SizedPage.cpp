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

#include <alaska/SizedPage.hpp>
#include <alaska/SizeClass.hpp>
#include <alaska/Logger.hpp>
#include <alaska/SizedAllocator.hpp>
#include <string.h>
#include <ck/template_lib.h>

namespace alaska {


  SizedPage::~SizedPage() { return; }

  void *SizedPage::alloc(const alaska::Mapping &m, alaska::AlignedSize size) {
    auto *header = (ObjectHeader *)allocator.alloc();
    if (unlikely(header == nullptr)) return nullptr;
    header->set_mapping(&m);
    header->set_object_size(size);
    return header->data();
  }


  bool SizedPage::release_local(const alaska::Mapping &m, void *ptr) {
    auto header = alaska::ObjectHeader::from(ptr);
    allocator.release_local((void *)header);
    return true;
  }


  bool SizedPage::release_remote(const alaska::Mapping &m, void *ptr) {
    auto header = alaska::ObjectHeader::from(ptr);
    allocator.release_remote((void *)header);
    return true;
  }



  void SizedPage::set_size_class(int cls) {
    this->size_class = cls;

    // The size of the object.
    this->object_size = alaska::class_to_size(this->size_class);
    // The size of the block, which includes the header
    size_t real_size = this->object_size + sizeof(ObjectHeader);
    // The number of objects (and headers) that can fit in this page.
    this->capacity = (double)alaska::page_size / real_size;
    this->live_objects = 0;

    snprintf(name, sizeof(name), "Sized(%zu)", this->object_size);

    // initialize
    allocator.configure(this->memory, real_size, capacity);
  }


  // The goal of this function is to take a fragmented heap, and
  // apply a simple two-finger compaction algorithm.  We start with
  // a heap that looks like this (# is allocated, _ is free)
  //  [###_##_#_#__#_#_#__#]
  // and have pointers to the start and end like this:
  //  [###_##_#_#__#_#_#__#]
  //   ^                  ^
  // We iterate until those pointers are the same. If the left
  // pointer points to an allocated object, we increment it. If the
  // right pointer points to a free object (or a pinned), it is
  // decremented.  If neither pointer changes, the left points to a
  // free object and the right to an allocated one, we swap their
  // locations then inc right and dec left.
  //
  // When this process is done you should have a heap that looks
  // like this:
  //  [###########_________]
  // The last step in this process is to "reset" the free list to be
  // empty and for "expansion" to begin at the end of the allocated
  // objects. Then, if deemed beneficial, you can use MADV_DONTNEED
  // to free memory back to the kernel.
  //
  // This function then returns how many objects it moved
  long SizedPage::compact(void) {
    // The return value - how many objects this function has moved.
    long moved_objects = 0;

    // The first step is to clear the free list so that we don't have any
    // corruption problems. Later we will update it to point at the end of
    // the allocated objects.
    allocator.reset_free_list();




    // This algorithm is a two-finger walk. We have two pointers, one
    // pointing to the left (destination) and one pointing to the right
    // (source). We walk the left pointer forward until we find a free slot.
    // We then walk the right pointer forward starting at the left (or where
    // the right used to be, whichever is greater). If the right pointer
    // points to a used slot slot, we move that object to the location of the
    // left pointer and add the right pointer to the free list.
    //
    // One complication here is with object pinning, so the right pointer
    // considers pinned objects to be free, so it skips over them. If the
    // right pointer reaches the end location, we walk the left pointer
    // over the remainder of the heap, and any free slots are added to the
    // free list.

    off_t left = 0;
    off_t right = 0;
    off_t end = allocator.object_extent();
    off_t last_object_seen = -1;

    // A helper function to get the object header at a given index.
    // This just cleans up the code.
    auto get_object = [&](off_t index) -> ObjectHeader * {
      return (ObjectHeader *)((off_t)this->memory + index * (object_size + sizeof(ObjectHeader)));
    };

    // This is a helper function I used while debugging. Not gonna remove it
    // incase something else pops up.
    /*
    auto dump = [&](const char *msg) {
      printf("%15s: ", msg);
      for (off_t i = 0; i < end; i++) {
        auto *header = get_object(i);

        if (i == left) {
          printf("\e[32m[");
        } else {
          printf(" ");
        }

        if (allocator.is_free(header)) {
          printf(". . . . .");
        } else {
          if (i == last_object_seen) {
            printf("!");
          } else {
            printf(" ");
          }
          printf("%08lx", (unsigned long)header->get_mapping()->handle_id());
        }

        if (i == right) {
          printf("]\e[0m");
        } else {
          printf(" ");
        }
      }
      if (right == end) {
        printf("  >\e[0m");
      }
      printf("\n");
    };
    */


    // first, run the end back until we find an allocated object. This sets the
    // bounds for the right pointer. (It optimizes for SizedAllcoator::extend()
    // being called with a large value)
    while (end > 0) {
      auto obj = get_object(end - 1);
      if (allocator.is_allocated(obj)) {
        break;
      }
      end--;
      // dump("end wb");
    }

    while (left < end) {
      // if the right pointer is less than the left, we need to set it to
      // the left + 1, so we can move the object. This maintains that invariant
      if (right <= left) {
        right = left + 1;
      }
      // dump("top of loop");

      auto left_obj = get_object(left);
      if (allocator.is_allocated(left_obj)) {
        left++;
        continue;
      }


      if (right == end) {
        // If the right pointer sees the end of the heap, we need to walk it back until
        // it finds an allocated object. That spot + 1 is the new end.
        // We then walk the left to that end and add any free slots found to the
        // free list.

        while (right > left) {
          // dump("ec backup");
          if (allocator.is_allocated(get_object(right))) {
            break;
          }
          right--;
        }

        end = right;

        while (left < end) {
          // dump("ec top");
          auto left_obj = get_object(left);
          if (allocator.is_free(left_obj)) {
            allocator.release_local(left_obj);
          }
          // if the left pointer points to an allocated object, we can't do anything so
          // walk it forward (towards the right)
          left++;
        }
        break;
      }



      // we found a free slot. we need to the object to the right to move here.
      bool found_right = false;  // Did we find a valid object to move?
      while (right < end) {
        // dump("right loop");
        // if the right pointer points to a free slot, we can't do anything so
        // move it forward
        auto right_obj = get_object(right);
        if (allocator.is_free(right_obj)) {
          // TODO: make this the spot we "skip" to next time we need to pick a left value.
          right++;
          continue;
        }
        // if the right pointer points to a pinned object, we can't do anything
        // so move it forward
        auto handle_mapping = right_obj->get_mapping();
        if (handle_mapping->is_pinned()) {
          right++;
          continue;
        }

        // break from the loop if we have a valid object to move.
        found_right = true;
        break;
      }


      if (found_right) {
        // dump("found right");

        auto obj_r = get_object(right);
        auto map_r = obj_r->get_mapping();

        auto obj_l = get_object(left);
        auto map_l = obj_l->get_mapping();


        // copy the object's data.
        memcpy(obj_l->data(), obj_r->data(), object_size);

        obj_r->set_mapping(0);  // clear the right mapping
        map_r->set_pointer(obj_l->data());
        obj_l->set_mapping(map_r);

        allocator.track_freed(obj_r);
        allocator.track_allocated(obj_l);
        moved_objects++;



        // now. move!
      }
    }


    // end = last_object_seen + 1;
    // dump("end of loop");
    allocator.reset_bump_allocator(get_object(end));

    return moved_objects;
  }




  long SizedPage::jumble(void) {
    return 0;
#if 0
    char buf[this->object_size];  // BAD

    // Simple two finger walk to swap every allocation
    long left = 0;
    long right = capacity - 1;
    long swapped = 0;

    while (right > left) {
      auto *lo = ind_to_header(left);
      auto *ro = ind_to_header(right);

      auto *rm = ro->get_mapping();
      auto *lm = lo->get_mapping();

      if (rm == NULL or rm->is_pinned()) {
        right--;
        continue;
      }

      if (lm == NULL or lm->is_pinned()) {
        left++;
        continue;
      }


      // swap the handles!

      // Grab the two pointers
      void *left_ptr = ind_to_object(left);
      void *right_ptr = ind_to_object(right);

      // Swap their data
      memcpy(buf, left_ptr, object_size);
      memcpy(left_ptr, right_ptr, object_size);
      memcpy(right_ptr, buf, object_size);

      // Tick the fingers
      left++;
      right--;


      // Swap the mappings and whatnot
      auto lss = lo->size_slack;
      auto rss = ro->size_slack;

      rm->set_pointer(left_ptr);
      ro->set_mapping(lm);
      ro->size_slack = lss;

      lm->set_pointer(right_ptr);
      lo->set_mapping(rm);
      lo->size_slack = rss;

      swapped++;
    }

    return swapped;
#endif
  }



}  // namespace alaska
