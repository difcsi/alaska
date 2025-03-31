#pragma once

#include <stdlib.h>

#ifndef _NEW
inline void *operator new(size_t, void *ptr) noexcept { return ptr; }
#endif
