/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */


#pragma once


namespace alaska {

  // This template class allows another class to record "I am owned by
  // some other instance of type T", then this can be queried later.
  template <typename T>
  class OwnedBy {
   public:
    OwnedBy(void)
        : m_current_owner(nullptr) {}

    inline T *get_owner(void) const { return m_current_owner; }
    inline void set_owner(T *new_owner) { this->m_current_owner = new_owner; }
    bool is_owned_by(T *other) const { return other == m_current_owner; }

   private:
    T *m_current_owner;
  };
}  // namespace alaska
