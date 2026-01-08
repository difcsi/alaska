#include <alaska/Domain.hpp>
#include <alaska/Runtime.hpp>
#include <alaska/HandleTable.hpp>


namespace alaska {
  Domain::Domain(HandleTable &ht)
      : ht(ht) {}

  Domain::~Domain() {
    // TODO: Handle case where slabs still have active allocations
    dropAll();
  }

  alaska::HandleSlab *Domain::find_next_slab(void) {
    // Search existing slabs for one with free space
    for (auto slab : slabs) {
      if (slab->has_any_free()) {
        return slab;
      }
    }

    // No existing slab has free space, allocate a fresh one
    // Pass this Domain by reference to fresh_slab (enforces ownership invariant)
    alaska::HandleSlab *new_slab = ht.fresh_slab(*this);
    if (new_slab != nullptr) {
      slabs.push(new_slab);
    }
    return new_slab;
  }

  void Domain::dropAll(void) {
    // Return all slabs back to the handle table's free list
    for (auto slab : slabs) {
      // TODO: Need to release allocated handles and data when slabs are dropped
      ht.return_slab(slab);
    }
    slabs.clear();
    current_slab = nullptr;
  }

  alaska::Mapping *Domain::alloc_handle(void) {
    // Ensure we have a current slab
    if (current_slab == nullptr || !current_slab->has_any_free()) {
      current_slab = find_next_slab();
    }

    if (current_slab == nullptr) {
      return nullptr;
    }

    return current_slab->alloc();
  }


  // Default domain implementation of handle_fault does nothing.
  bool Domain::handle_fault(alaska::Mapping &m) {
    // alaska::printf("Domain handling fault for mapping %p\n", &m);
    m.set_fault_pending(false);
    return true;
  }

}  // namespace alaska


extern "C" alaska_domain_t *alaska_domain_create(struct alaska_domain_config *cfg) {
  // Get the global runtime
  alaska::Runtime &runtime = alaska::Runtime::get();
  // We need a reference to the Handle Table to create the domain
  alaska::HandleTable &ht = runtime.handle_table;

  // Allocate the domain
  alaska::Domain *domain = new alaska::Domain(ht);

  // Return it as an opaque pointer
  return reinterpret_cast<alaska_domain_t *>(domain);
}

extern "C" void alaska_domain_destroy(alaska_domain_t *a) {
  if (a == NULL) return;
  // Grab the domain pointer from the opaque type and call delete on it.
  delete reinterpret_cast<alaska::Domain *>(a);
}


extern "C" void *alaska_domain_alloc(alaska_domain_t *a, size_t size) {
  if (a == NULL) return NULL;
  // Grab the domain pointer from the opaque type
  alaska::Domain *domain = reinterpret_cast<alaska::Domain *>(a);

  auto *tc = alaska::ThreadCache::current();

  return tc->halloc(*domain, size);
}

extern "C" void alaska_domain_free(alaska_domain_t *a, void *ptr) {
  if (a == nullptr) return;
  alaska::Domain *domain = reinterpret_cast<alaska::Domain *>(a);
  (void)ptr;

  // for now, we just defer to the thread cache free, until having the
  // domain free means something.
  auto *tc = alaska::ThreadCache::current();
  tc->hfree(ptr);
}
