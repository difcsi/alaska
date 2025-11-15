#pragma once

#include <alaska.h>  // for alaska_domain_t and alaska_domain_* functions
#include <alaska/alaska.hpp>
#include <alaska/HandleTable.hpp>


namespace alaska {

  class ThreadCache;


  // A Domain in Alaska represents an isolated partition of handle table slabs.
  // Each domain manages its own set of handle table slabs independently.
  // By default, there is a "global" domain used when no other domain is specified.
  //
  // Slab Lifecycle:
  // - Domain requests slabs from HandleTable via fresh_slab()
  // - Domain tracks all its slabs in the 'slabs' vector
  // - Domain allocates from current_slab until exhausted
  // - When exhausted, Domain calls find_next_slab() to get another slab
  // - find_next_slab() searches existing slabs or requests fresh ones
  // - When Domain is destroyed, dropAll() returns all slabs to HandleTable
  //
  // Ownership:
  // - Domains own the slabs in their 'slabs' vector
  // - Slabs have owner_domain pointer pointing back to their owning Domain
  // - When a slab is returned to HandleTable free list, owner_domain is cleared
  //
  // This is the actual Domain class that is returned by alaska_domain_create.
  class Domain : public alaska::InternalHeapAllocated {
   public:
    Domain(const Domain &) = delete;
    Domain(Domain &&) = delete;
    Domain &operator=(const Domain &) = delete;
    Domain &operator=(Domain &&) = delete;
    Domain(HandleTable &ht);
    ~Domain();

    // Allocate a new mapping from this domain's slice of the handle table.
    alaska::Mapping *alloc_handle(void);

    // Return all slabs owned by this domain back to the handle table.
    void dropAll(void);

    // Get the vector of slabs owned by this domain (for testing/introspection)
    const ck::vec<alaska::HandleSlab *> &get_slabs(void) const { return slabs; }

    // Get the current slab for this domain (for testing/introspection)
    alaska::HandleSlab *get_current_slab(void) const { return current_slab; }

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
