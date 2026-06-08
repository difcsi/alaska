/*
 * Global operator new / delete for the Alaska runtime.
 *
 * Some core runtime code (e.g. the cycle collector and ck:: containers) uses
 * plain `new`/`delete`, which lower to the global C++ allocation operators.
 * The runtime is built with -nostdinc++ and links no C++ runtime, so those
 * operators are otherwise undefined.
 *
 * We must NOT satisfy them from libstdc++ / the global malloc: the global
 * malloc is the very allocator Alaska interposes, so routing the runtime's own
 * internal objects through it risks reentrancy. Instead we back them with
 * alaska_internal_malloc/free, exactly like alaska::InternalHeapAllocated.
 *
 * The replaceable global operator new/delete are implicitly declared with
 * default visibility, so they cannot be redefined with hidden visibility via an
 * attribute. To avoid exporting a process-wide operator new that would hijack
 * the application's own C++ allocations, libalaska_core's link localizes these
 * symbols with a version script (see runtime/CMakeLists.txt).
 */

extern "C" void *alaska_internal_malloc(__SIZE_TYPE__ sz);
extern "C" void alaska_internal_free(void *ptr);

void *operator new(__SIZE_TYPE__ sz) { return alaska_internal_malloc(sz); }
void *operator new[](__SIZE_TYPE__ sz) { return alaska_internal_malloc(sz); }

void operator delete(void *ptr) noexcept { alaska_internal_free(ptr); }
void operator delete[](void *ptr) noexcept { alaska_internal_free(ptr); }

/* Sized-deallocation forms (C++14): route to the same internal free. */
void operator delete(void *ptr, __SIZE_TYPE__) noexcept { alaska_internal_free(ptr); }
void operator delete[](void *ptr, __SIZE_TYPE__) noexcept { alaska_internal_free(ptr); }
