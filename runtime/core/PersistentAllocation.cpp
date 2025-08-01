#include <alaska/PersistentAllocation.h>
#include <alaska/Arena.hpp>

#include <alaska/LazyGlobal.hpp>

namespace alaska {
  LazyGlobal<Arena> persistent_allocation_arena;

  void *PersistentAllocation::operator new(size_t size) {
    return persistent_allocation_arena->push(size);
  }

  void PersistentAllocation::operator delete(void *ptr) {
    alaska::printf("Invalid call to delete made for persistent allocation object at %p\n", ptr);
  }


}  // namespace alaska