#include <alaska/util/PersistentAllocation.h>
#include <alaska/util/Arena.hpp>

#include <alaska/util/LazyGlobal.hpp>

namespace alaska {
  static LazyGlobal<Arena> persistent_allocation_arena;

  void *PersistentAllocation::operator new(size_t size) {
    return persistent_allocation_arena->push(size);
  }

  void PersistentAllocation::operator delete(void *ptr) {
    // alaska::printf("Invalid call to delete made for persistent allocation object at %p\n", ptr);
  }


}  // namespace alaska