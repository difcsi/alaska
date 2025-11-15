# Domain Refactor Plan

## Architecture Changes

**New Ownership Model:**
- `HandleTable` → allocates slabs (bump/freelist only)
- `Domain` → owns and manages slabs (tracks list, handles slow path)
- `ThreadCache` → requests slab from Domain each time, doesn't store it

**Key Design Decisions:**
1. ThreadCache removes `handle_slab` member, fetches from `Domain&` on each allocation
2. halloc() already accepts optional `Domain&` parameter (existing interface work)
3. Slabs stay within their Domain when exhausted (domain-scoped recycling)
4. Domain uses simple slab list (no empty/partial/full queues initially)
5. **No mutex needed in Domain** - other system invariants guarantee safety

## Implementation Steps

### Phase 1: Domain Slab Management (Core)

**1.1 Update Domain class** (`runtime/alaska/Domain.hpp` & `.cpp`)
- Add `ck::vec<HandleSlab*> slabs` to track owned slabs
- Add `HandleSlab* current_slab` for active allocation
- Implement `get_slab()` method:
  - Returns `current_slab` if it has space
  - Otherwise, search `slabs` for one with free space
  - If none found, request new from HandleTable via `ht.fresh_slab()`
  - Set `owner_domain` on acquired slabs
- Allocation from the Domain is by the `ThreadCache` directly
  accessing the `current_slab`
- Implement a method in `Domain` to find a new `current_slab` when it is empty.

**1.2 Update HandleTable** (`runtime/alaska/HandleTable.cpp`)
- Modify `fresh_slab()` signature to **require** `Domain&` parameter (NOT optional)
  - Remove ThreadCache parameter entirely
  - Enforces invariant: every slab is either Domain-owned or free
  - HandleSlab stores `owner_domain` as nullable pointer for freelist routing compatibility
- Inside `fresh_slab()`, set HandleSlab `owner` to `nullptr` with TODO to remove TC ownership
- Keep `new_slab()` for now but mark deprecated (will remove later)
- Document that slabs are now Domain-owned, not ThreadCache-owned
- Invariant: Every HandleSlab must have owner_domain set when created

### Phase 2: ThreadCache Integration

**2.1 Remove handle_slab member** (`runtime/alaska/ThreadCache.hpp`)
- Delete `alaska::HandleSlab *handle_slab` member
- Update constructor to not initialize it

**2.2 Update allocation paths** (`runtime/alaska/ThreadCache.cpp`)
- Modify `new_mapping()` to accept `Domain& domain` parameter
  - Change `handle_slab->alloc()` to `domain.get_slab()->alloc()`
- Modify `new_mapping_slow_path()` to accept `Domain& domain`
  - Remove HandleTable slab acquisition code
  - Replace with `domain.get_slab()` call (Domain handles slow path)
- Update `halloc()` fast path (lines 183-208) to use domain parameter
  - Ask the domain for the mappin in the slow path.

**2.3 Update other ThreadCache methods**
- `free_mapping()` - should still work (routes to HandleSlab's release methods)
- Any other methods referencing `handle_slab` need domain parameter

### Phase 3: Runtime Integration

**3.1 Verify global_domain setup** (`runtime/alaska/Runtime.cpp`)
- Ensure `global_domain` is initialized properly
- Verify it's available before any ThreadCache allocations

### Phase 4: Eliminate HandleTable Slab Management Logic

**Goal**: Simplify HandleTable to ONLY allocate slabs (from free list or bump allocator). Remove domain-aware searching. Domains manage their own slab lifecycle.

**4.1 Add Free List Infrastructure to HandleTable**
- Add `HandleSlabQueue m_free_slabs;` member to track returned slabs
- Implement `HandleTable::return_slab(HandleSlab *slab)`:
  - Take lock
  - Full reset of slab state:
    - Clear `owner_domain` to nullptr
    - Reset free lists (clear local/remote entries)
    - Reset bump pointer (`next_free = start`)
  - Add TODO comment: "Need to release allocated handles and data when dropped"
  - Push slab to `m_free_slabs` queue
- Modify `HandleTable::fresh_slab()` to check free list first:
  - Try `m_free_slabs.pop()` before bump allocating
  - If found, set `owner_domain` and return
  - Otherwise, allocate new slab as before
- Update `HandleTable::dump()` to show free slab count

**4.2 Update Domain to Return Slabs**
- Modify `Domain::dropAll()`:
  - Loop through all slabs in `slabs` vector
  - Call `ht.return_slab(slab)` for each slab
  - Clear the vector and set `current_slab = nullptr`
- Verify `~Domain()` calls `dropAll()` (should already)

**4.3 Remove new_slab() Method**
- Verify no code calls `HandleTable::new_slab()`:
  - Expected: Only `fresh_slab()` is used
- Remove method declaration from `HandleTable.hpp`
- Remove method implementation from `HandleTable.cpp` (lines ~154-180)
- Remove TODO comment about "PERFORMANCE BAD HERE"

**4.4 Update Comments & Documentation**
- Update `HandleTable` class comment to document free list mechanism
- Clarify that `owner_domain` is the primary ownership indicator
- Note that HandleSlab's `owner` field is only for freelist routing, NOT ownership
- Update `Domain` class comment to document slab lifecycle

**Design Decisions**:
- Keep `HandleTable::m_slabs` vector for index-based lookup and global iteration
- Use `HandleSlabQueue` for free slab tracking (linked list, O(1) pop/push)
- Keep simple linear search in `Domain::find_next_slab()` (no empty/partial/full queues yet)
- Full slab reset when returned to free list (safe, but slower)
- Add TODO for future handle/data cleanup logic


## Key Files Modified

- `runtime/alaska/Domain.hpp` - Add slab list, current_slab, get_slab()
- `runtime/alaska/Domain.cpp` - Implement slab management logic
- `runtime/alaska/ThreadCache.hpp` - Remove handle_slab member
- `runtime/alaska/ThreadCache.cpp` - Add domain parameters to allocation methods
- `runtime/alaska/HandleTable.cpp` - Update fresh_slab() for domain ownership
- `runtime/alaska/HandleTable.hpp` - Update HandleSlab owner_domain semantics

## Success Criteria

- ✓ ThreadCache no longer stores handle_slab
- ✓ Domain manages slab allocation and recycling
- ✓ HandleTable only does bump/freelist slab allocation
- ✓ Slabs stay within their Domain
- ✓ halloc() works with both explicit domain and global_domain
- ✓ Performance unchanged (same fast path, just fetches from domain)

---

## PHASE 4 COMPLETED ✓

**Phase 4: Eliminate HandleTable Slab Management Logic**

All components successfully implemented and tested:
- ✓ Free list infrastructure added to HandleTable
- ✓ Domain returns slabs via dropAll()
- ✓ new_slab() method removed entirely
- ✓ Fresh_slab() checks free list before bump allocating
- ✓ HandleSlab::reset() method added for proper state reset
- ✓ Full slab reset on return_slab() with TODO for handle cleanup
- ✓ Documentation updated for new ownership model
- ✓ ThreadCache::hfree() fixed for Domain ownership model
- ✓ All 90 runtime tests pass

Key improvements:
- Eliminates O(n) domain-aware searching
- HandleTable now focuses solely on slab allocation/recycling
- Domains fully manage their own slab lifecycle
- Free list enables efficient slab reuse
- Handles properly reused within domains after free

---

## DOMAIN REFACTOR COMPLETE ✓

All four phases of the Domain refactor have been successfully implemented,
tested, and committed:

**Phase 1: Domain Slab Management** - Domain owns slab list, manages
allocation and recycling within domain scope.

**Phase 2: ThreadCache Integration** - ThreadCache removed handle_slab
member, now fetches current_slab from Domain on each allocation.

**Phase 3: Runtime Integration** - Global domain properly initialized
and available before any ThreadCache allocations.

**Phase 4: Eliminate HandleTable Logic** - HandleTable simplified to
only manage bump allocation and free list. All slab lifecycle management
delegated to Domains.

**Final State:**
- No per-slab ThreadCache ownership
- Domain-scoped slab lifecycle
- Free list enables slab recycling across allocation/deallocation cycles
- Clean separation of concerns:
  - HandleTable: bump allocator + free list management
  - Domain: slab ownership + lifecycle management
  - ThreadCache: allocation/deallocation via domain
