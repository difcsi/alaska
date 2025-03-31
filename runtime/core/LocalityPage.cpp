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

#include <alaska/LocalityPage.hpp>
#include <alaska/Heap.hpp>


namespace alaska {


  void *LocalitySlab::alloc(size_t size, const alaska::Mapping &m) {
    size = round_up(size, 8);
    auto required = size + sizeof(alaska::ObjectHeader);
    if (unlikely(required > available())) return nullptr;

    auto header = (alaska::ObjectHeader *)(data + bump_size);
    bump_size += required;

    header->set_mapping(&m);
    header->set_object_size(size);
    header->localized = true;

    return header->data();
  }

  void LocalitySlab::free(void *ptr) {
    auto md = (alaska::ObjectHeader *)((off_t)ptr - sizeof(alaska::ObjectHeader));
    md->set_mapping(NULL);  // Freeing is trival - just say it has no handle id assigned.
    freed += md->object_size() + sizeof(alaska::ObjectHeader);
  }

  size_t LocalitySlab::get_size(void *ptr) {
    auto md = (alaska::ObjectHeader *)((off_t)ptr - sizeof(alaska::ObjectHeader));
    return md->object_size();
  }


  LocalityPage::~LocalityPage() {}

  void *LocalityPage::alloc(const alaska::Mapping &m, alaska::AlignedSize size) {
    if (current_slab == nullptr) {
      printf("allocating new slab\n");
      current_slab = allocate_slab();
    }

    void *ptr = current_slab->alloc(size, m);
    if (unlikely(ptr == nullptr)) {
      current_slab = allocate_slab();
      if (current_slab == nullptr) {
        return nullptr;
      }
      // try again.
      return this->alloc(m, size);
    }
    return ptr;
  }

  // TODO:
  bool LocalityPage::release_local(const alaska::Mapping &m, void *ptr) {
    // defer to the slab
    auto slab = get_slab(ptr);
    slab->free(ptr);
    return true;
  }

}  // namespace alaska
