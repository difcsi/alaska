#pragma once

#include <alaska/Logger.hpp>
#include <type_traits>
#include <new>

namespace alaska {
  // Sometimes, alaska is called before global constructors are run. This
  // means that globals are not initialized yet, so we need to instead
  // use a "lazy" global which initializes when they are accessed, not when
  // the program starts.

  template<typename T>
  class LazyGlobal {

    T *m_instance;
    uint8_t m_storage[sizeof(T)];

    public:
    T &get() {
      if (m_instance == nullptr) {
        // alaska::printf("LazyGlobal: Initializing instance of type %s at %p\n", __PRETTY_FUNCTION__, m_storage);
        m_instance = new (m_storage) T();
      }
      return *m_instance;
    }

    T &operator*() {
      return get();
    }
    T *operator->() {
      return &get();
    }
  };

  // static assert that LazyGlobal is a PoD type.
  static_assert(std::is_pod<LazyGlobal<int>>::value, "LazyGlobal must be a POD type");
};  // namespace alaska
