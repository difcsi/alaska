#include <alaska/Domain.hpp>
#include <alaska/Runtime.hpp>


namespace alaska {
  //


  Domain::Domain(HandleTable &ht)
      : ht(ht) {}


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
  // Grab the domain pointer from the opaque type and call delete on it.
  delete reinterpret_cast<alaska::Domain *>(a);
}


extern "C" void *alaska_domain_alloc(alaska_domain_t *a, size_t size) {
  // Grab the domain pointer from the opaque type
  alaska::Domain *domain = reinterpret_cast<alaska::Domain *>(a);
  // TODO: figure out allocation.
  // return domain->alloc(size);
  return NULL;
}

extern "C" void alaska_domain_free(alaska_domain_t *a, void *ptr) {
  alaska::Domain *domain = reinterpret_cast<alaska::Domain *>(a);

  // for now, we just defer to the thread cache free.

  // TODO: free
  // domain->free(ptr);
}
