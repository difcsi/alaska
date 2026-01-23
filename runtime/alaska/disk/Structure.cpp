/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <alaska/util/utils.h>
#include <ck/map.h>
#include <ck/option.h>
#include <ck/vec.h>

#include <alaska/disk/BufferPool.hpp>
#include <alaska/disk/Structure.hpp>

namespace alaska::disk {

  Structure::Structure(BufferPool &pool, const char *name)
      : pool(pool)
      , root_page_id(getRootPageID(pool, name)) {
    strncpy(this->name, name, sizeof(this->name) - 1);
  }


}  // namespace alaska::disk
