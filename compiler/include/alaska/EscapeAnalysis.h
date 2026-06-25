#pragma once
// EscapeAnalysis - a small intraprocedural def-use walk that classifies how the
// address of a single allocation (an `alloca`, or the result of a malloc-family
// call) is used within its defining function.
//
// It is shared by two passes that ask opposite questions of the same walk:
//
//   * AlaskaStackPromotePass promotes an address-taken `alloca` to a `halloc`
//     handle. It promotes when the address escapes (`taken`).
//
//   * AlaskaReplacementPass keeps a non-escaping `malloc`/`calloc` raw instead of
//     turning it into a handle. It keeps raw when the address does NOT escape.
//
// Keeping one analyzer means the two passes share an identical notion of
// "escape".

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Value.h>

#include <set>
#include <string>

namespace alaska {

// Classification of how an allocation's address is used.
struct EscapeInfo {
  bool taken = false;     // address escapes the object in any way
  bool toHinted = false;  // address flows into a hinted function's pointer arg
  bool returned = false;  // address is returned from the function
  bool unsafe = false;    // used in a way we must not touch (atomics, va_list, inline asm)

  // lifetime/invariant markers on the object; meaningless once it is on the
  // heap, so StackPromote removes them at promotion time.
  llvm::SmallPtrSet<llvm::IntrinsicInst *, 4> lifetimeMarkers;

  // When analyzeEscape runs with `allocatorCallsAreUses`, calls that *consume*
  // the pointer as a deallocation/size query (free/hfree/malloc_usable_size/
  // alaska_usable_size) are recorded here instead of being treated as escapes,
  // so the caller can keep those calls on the same allocator as the allocation.
  llvm::SmallPtrSet<llvm::CallBase *, 4> deallocUses;

  // When `followReallocChain` is set, realloc/hrealloc calls reached at their
  // pointer slot are recorded here (to be kept on the same allocator), and their
  // results are followed as a continuation of the same logical allocation.
  llvm::SmallPtrSet<llvm::CallBase *, 4> reallocUses;

  // The set of values that "produce" the allocation: the root plus every
  // realloc result followed via `followReallocChain`. A consuming call (free /
  // realloc / usable_size) is only safe to keep raw if its pointer traces back
  // to one of these. Always contains `root`.
  llvm::SmallPtrSet<llvm::Value *, 8> regionProducers;
};

// Walk the def-use chain of `root` (an alloca or a malloc-family call result),
// following pointer-producing transient instructions (bitcast / gep /
// addrspacecast / phi / select), and classify every use into `EscapeInfo`.
//
// `hints` (optional): when non-null, also record whether the address reaches a
// call to one of those functions as a pointer argument (StackPromote heap-hints).
//
// `allocatorCallsAreUses`: when false (default) every call that takes the
// address is an escape -- StackPromote's original behavior. When true, calls to
// free/hfree/malloc_usable_size/alaska_usable_size at their pointer slot (arg 0)
// are recorded in `deallocUses` rather than counted as escapes.
//
// `followReallocChain`: when true, realloc/hrealloc at the pointer slot is
// recorded in `reallocUses` and its result is followed (the realloc result joins
// `regionProducers`); when false, realloc is treated as an ordinary escape.
//
// `checkNoCapture`: when true, a pointer passed to a call argument that the
// callee provably does not capture (an explicit `nocapture` attribute, or -- one
// level deep -- a defined callee whose corresponding parameter does not escape
// its body) is treated as a non-escaping use rather than an escape (keep-raw
// case (b)).
EscapeInfo analyzeEscape(llvm::Value *root, const std::set<std::string> *hints,
    bool allocatorCallsAreUses = false, bool followReallocChain = false,
    bool checkNoCapture = false);

}  // namespace alaska
