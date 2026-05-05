#pragma once


#include <stdlib.h>

#if __has_include(<new>)
#include <new>
#else
inline void *operator new(size_t, void *ptr) noexcept {
  return ptr;
}
inline void operator delete(void *, void *) noexcept {}
#endif
