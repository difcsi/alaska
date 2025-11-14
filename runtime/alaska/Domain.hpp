#pragma once

#include <alaska.h>  // for alaska_domain_t and alaska_domain_* functions
#include <alaska/alaska.hpp>
#include <alaska/HandleTable.hpp>


namespace alaska {

  class ThreadCache;


  // A Domain in Alaska represents an isolated allocation of the
  // handle table. Each domain in the system has its own set of handle
  // table slabs which it manages independently. By default, there is
  // a "global" domain which is used when no other domain is
  // specified.
  //
  // Handle slabs are owned by a domain, and each domain can allocate
  // and free slabs as needed
  //
  // This is the actual Domain class that is returned by alaska_domain_create.
  class Domain : public alaska::InternalHeapAllocated {
   public:
    Domain(const Domain &) = default;
    Domain(Domain &&) = default;
    Domain &operator=(const Domain &) = delete;
    Domain &operator=(Domain &&) = delete;
    Domain(HandleTable &ht);
    ~Domain();

    // Allocate a new mapping from this domain's slice of the handle table.
    alaska::Mapping *alloc_handle(void);

    // Return all slabs owned by this domain back to the handle table.
    void dropAll(void);

   protected:
    friend alaska::ThreadCache;  // The ThreadCache is allowed to access the handle slab.

    // The current handle slab for allocation from this Domain.
    // ThreadCache accesses this directly for fast allocation path.
    alaska::HandleSlab *current_slab = nullptr;

    // Find a new current_slab when the existing one is exhausted.
    // Returns nullptr if no slabs have free space and no new slab can be allocated.
    alaska::HandleSlab *find_next_slab(void);

   private:
    // All handle slabs owned by this domain.
    ck::vec<alaska::HandleSlab *> slabs;

    // The handle table we are allocating from.
    //    TODO: do we need to store this here? Maybe refactor to have
    //    an explicitly global handle table.
    alaska::HandleTable &ht;
  };



}  // namespace alaska
