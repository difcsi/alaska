#pragma once

#include <pthread.h>

namespace ck {


  class mutex {
   private:
    int locked = 0;

   public:
    mutex(void) = default;

    void init(void) { locked = 0; }
    ~mutex(void) = default;

    int try_lock(void) { return !__atomic_test_and_set(&this->locked, __ATOMIC_ACQUIRE); }

    void lock(void) {
      while (__atomic_test_and_set(&this->locked, __ATOMIC_ACQUIRE)) {
      }
    }

    void unlock(void) { __atomic_clear(&this->locked, __ATOMIC_RELEASE); }
    
  };


  class scoped_lock {
    ck::mutex &lck;
    bool locked = false;

   public:
    inline scoped_lock(ck::mutex &lck)
        : lck(lck) {
      lck.lock();
    }

    inline ~scoped_lock(void) { lck.unlock(); }
  };

}  // namespace ck
