#pragma once

#include <stdint.h>
#include <stdlib.h>

namespace alaska {


  // Inherit from this class to use the persistent allocation arena.
  // Objects which are allocated using this class *cannot* be freed.
  class PersistentAllocation {
    public:
    void *operator new(size_t size);
    void operator delete(void *ptr);
  };

}