#pragma once

#include <alaska/Logger.hpp>
#include <new>

namespace alaska {
  // Sometimes, alaska is called before global constructors are run. This
  // means that globals are not initialized yet, so we need to instead
  // use a "lazy" global which initializes when they are accessed, not when
  // the program starts.

  template<typename T>
  class LazyGlobal {

    T *m_instance = nullptr;
    uint8_t m_storage[sizeof(T)];

    public:
    T &get() {
      if (m_instance == nullptr) {
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
};  // namespace alaska