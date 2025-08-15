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


  LocalityPage::~LocalityPage() {}

  void *LocalityPage::alloc(const alaska::Mapping &m, alaska::AlignedSize size) {
    size_t real_size = size + sizeof(alaska::ObjectHeader);
    if (unlikely(real_size >= available())) {
      // alaska::printf(
      //     "Locality page could not allocated %zu bytes (only %zu available, %zu committed, %zu "
      //     "freed)\n",
      //     size, available(), committed(), freed_bytes);
      return nullptr;
    }

    // alaska::printf(
    //     "Locality page %p allocating %zu bytes (of %zu available, %zu committed, %zu freed)\n",
    //     this, size, available(), committed(), freed_bytes);

    // Bump allocate!
    alaska::ObjectHeader *header =
        (alaska::ObjectHeader *)__builtin_assume_aligned((void *)bump_next, 16);
    header->set_mapping(&m);
    header->set_object_size(size);
    header->localized = 1;

    bump_next = (void *)((uintptr_t)bump_next + header->real_object_size());

    return header->data();
  }

  // TODO:
  bool LocalityPage::release_local(const alaska::Mapping &m, void *ptr) {
    auto *header = alaska::ObjectHeader::from(ptr);
    this->freed_bytes += header->real_object_size();

    header->set_mapping(nullptr);  // This is how we free.
    return true;
  }

}  // namespace alaska
