#include <alaska/Passes.h>
#include <alaska/RefcountElision.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;
// underlyingThroughTranslate / baseAndConstOffset are shared with RefcountInc and now
// live in alaska/RefcountElision.h.
using alaska::baseAndConstOffset;
using alaska::underlyingThroughTranslate;

// If `v` is the value produced by a zeroing allocator (halloc/hcalloc), return the
// call instruction producing it; else null. Alaska wraps allocations in GC
// statepoints, so the value is usually `gc.result(gc.statepoint(... @halloc ...))`
// rather than a direct call -- look through that. The returned CallBase is the SSA
// value the slot pointer is derived from.
static CallBase *zeroingAllocResult(Value *v) {
  auto *cb = dyn_cast<CallBase>(v);
  if (!cb) return nullptr;
  Function *callee = cb->getCalledFunction();
  if (callee && callee->getIntrinsicID() == Intrinsic::experimental_gc_result) {
    // gc.result(token); the token is the statepoint, whose operand 2 (after id and
    // num-patch-bytes) is the actual called function.
    auto *sp = dyn_cast<CallBase>(cb->getArgOperand(0));
    if (!sp || !sp->getCalledFunction() ||
        sp->getCalledFunction()->getIntrinsicID() != Intrinsic::experimental_gc_statepoint)
      return nullptr;
    callee = dyn_cast<Function>(sp->getArgOperand(2)->stripPointerCasts());
  }
  if (!callee) return nullptr;
  StringRef nm = callee->getName();
  return (nm == "halloc" || nm == "hcalloc") ? cb : nullptr;
}

// Is `base` an Alaska GC pin-set stack slot -- an alloca that is only written to and
// handed to GC statepoints (as deopt / gc-live root metadata), never LOADED back
// into the mutator? Alaska stores a freshly-allocated handle into such a slot to pin
// it across safepoints before its fields are initialized. That store cannot be used
// to write through the handle (nothing in the function ever loads the pinned copy),
// so it does not invalidate "this slot is still the allocator's null". Conservative:
// any load, escape, or non-statepoint call use makes it return false.
static bool isGCOnlyPinAlloca(Value *base) {
  if (!isa<AllocaInst>(base)) return false;
  SmallVector<Value *, 8> work{base};
  SmallPtrSet<Value *, 8> seen{base};
  while (!work.empty()) {
    Value *cur = work.pop_back_val();
    for (User *U : cur->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I) return false;
      if (isa<LoadInst>(I)) return false;  // loaded back -> the copy could be used to write.
      if (auto *st = dyn_cast<StoreInst>(I)) {
        if (st->getValueOperand() == cur) return false;  // the slot pointer itself escapes.
        continue;                                        // a store INTO the pin set: fine.
      }
      if (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I)) {
        if (seen.insert(I).second) work.push_back(I);
        continue;
      }
      if (auto *cb = dyn_cast<CallBase>(I)) {
        Function *f = cb->getCalledFunction();
        if (f && (f->getIntrinsicID() == Intrinsic::experimental_gc_statepoint ||
                  f->getIntrinsicID() == Intrinsic::lifetime_start ||
                  f->getIntrinsicID() == Intrinsic::lifetime_end))
          continue;  // GC root metadata / lifetime markers only.
        return false;
      }
      return false;
    }
  }
  return true;
}

// Is the dec barrier for store `S` provably redundant because the slot it
// overwrites still holds the zero (null) a zeroing allocator left there -- i.e.
// this is the first write to that slot? dec(null) is a guaranteed no-op, so the
// barrier (translate + load of the old value + call) can be elided.
//
// SOUND and conservative: a wrong "yes" would drop a real decrement and leak (then
// never reclaim) a handle, so every source of uncertainty returns false (keep the
// barrier). The slot is null at S iff:
//   (1) its base is a zeroing fresh allocation (halloc/hcalloc) that dominates S;
//   (2) S cannot re-run against that same allocation (no enclosing loop that does
//       not also re-run the alloc), so a later iteration can't see an earlier one's
//       store; and
//   (3) nothing that can execute *before* S writes that slot or lets the allocation
//       escape/alias. We verify (3) by walking every pointer derived from the alloc
//       and rejecting any use that can reach S and is not a plain load, a further
//       constant derivation, or a store through a constant-offset pointer to a
//       DIFFERENT slot. Position sensitivity (isPotentiallyReachable) is essential:
//       e.g. `node *n = halloc(); n->l = ...; return n;` escapes n via the return,
//       but that return runs only AFTER the field stores, so it does not disqualify
//       eliding their barriers.
static bool decOverwritesFreshNull(StoreInst *S, const DataLayout &DL,
                                   DominatorTree &DT, LoopInfo &LI) {
  unsigned bits = DL.getIndexTypeSizeInBits(S->getPointerOperand()->getType());
  APInt off(bits, 0);
  Value *base = baseAndConstOffset(S->getPointerOperand(), DL, off);
  // Base must be the result of a zeroing fresh allocation (halloc/hcalloc), possibly
  // through a GC statepoint wrapper. (hrealloc preserves old contents -- excluded.)
  CallBase *alloc = zeroingAllocResult(base);
  if (!alloc) return false;
  if (!DT.dominates(alloc, S)) return false;
  if (Loop *L = LI.getLoopFor(S->getParent()))
    if (!L->contains(alloc->getParent())) return false;

  auto reachesS = [&](Instruction *U) {
    return isPotentiallyReachable(U, S, /*ExclusionSet=*/nullptr, &DT, &LI);
  };

  SmallVector<Value *, 16> work;
  SmallPtrSet<Value *, 16> seen;
  work.push_back(alloc);
  seen.insert(alloc);
  while (!work.empty()) {
    Value *cur = work.pop_back_val();
    for (User *U : cur->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I) return false;  // ConstantExpr / non-instruction use -- give up.
      if (isa<LoadInst>(I)) continue;  // reading the allocation is harmless.
      if (auto *gep = dyn_cast<GetElementPtrInst>(I)) {
        // A variable index could land on our slot; only follow constant ones.
        if (!gep->hasAllConstantIndices()) {
          if (reachesS(gep)) return false;
          continue;
        }
        if (seen.insert(gep).second) work.push_back(gep);
        continue;
      }
      if (isa<BitCastInst>(I) || isa<AddrSpaceCastInst>(I)) {
        if (seen.insert(I).second) work.push_back(I);
        continue;
      }
      if (auto *call = dyn_cast<CallInst>(I)) {
        // alaska_translate is a pure address derivation; follow its result.
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "alaska_translate" &&
            call->arg_size() == 1 && call->getArgOperand(0) == cur) {
          if (seen.insert(call).second) work.push_back(call);
          continue;
        }
        // Any other call (incl. memset/memcpy, which are nocapture but WRITE) might
        // store into the slot before S. (A statepoint that takes the allocation as a
        // GC root would also land here and conservatively bail -- sound, just misses
        // the elision; revisit if it proves to block real cases.)
        if (reachesS(call)) return false;
        continue;
      }
      if (auto *st = dyn_cast<StoreInst>(I)) {
        if (st->getValueOperand() == cur) {  // the pointer is stored AS A VALUE.
          // Benign if it is just a pin into a GC-only stack pin set (Alaska pins a
          // freshly-allocated handle there before initializing its fields). Such a
          // pin cannot be used to write through the handle, so the slot stays null.
          if (isGCOnlyPinAlloca(getUnderlyingObject(st->getPointerOperand()))) continue;
          if (reachesS(st)) return false;  // a real escape that can run before S.
          continue;
        }
        if (st == S) continue;        // our own (initializing) store.
        if (!reachesS(st)) continue;  // cannot execute before S.
        // A store through a derived pointer that can run before S: safe only if it
        // provably targets a different constant slot of this same allocation.
        APInt o(bits, 0);
        Value *b = baseAndConstOffset(st->getPointerOperand(), DL, o);
        if (b != alloc) return false;  // unknown / possibly-aliasing target.
        if (o == off) return false;    // writes OUR slot before S.
        continue;                      // different field: harmless.
      }
      // ret / phi / select / ptrtoint / icmp / etc.: only disqualifies if it can
      // execute before S. The common `return n;` after the field inits cannot reach
      // S, so it is ignored; anything that can reach S is treated conservatively.
      if (reachesS(I)) return false;
    }
  }
  return true;
}

/**
 * RefcountDecPass - Inserts calls to decrement the refcount when a handle
 * is being overwritten. This ensures that when a memory location containing
 * a handle is replaced with a new value, the old handle's refcount is
 * properly decremented.
 */

class RefcountDecVisitor : public llvm::InstVisitor<RefcountDecVisitor> {
 public:
  llvm::Module *M;
  const llvm::DataLayout &DL;
  llvm::DominatorTree &DT;
  llvm::LoopInfo &LI;
  std::vector<std::pair<StoreInst *, Value *>> toInstrument;

  RefcountDecVisitor(llvm::Module *M, const llvm::DataLayout &DL, llvm::DominatorTree &DT,
                     llvm::LoopInfo &LI)
      : M(M), DL(DL), DT(DT), LI(LI) {}

  void visitStoreInst(llvm::StoreInst &I) {
    auto valueOperand = I.getValueOperand();
    auto pointerOperand = I.getPointerOperand();

    // Check if we're storing a pointer type (potential handle)
    if (!valueOperand->getType()->isPointerTy()) {
      return;
    }

    // Check if the pointer operand is also a pointer (handle to handle store)
    if (!pointerOperand->getType()->isPointerTy()) {
      return;
    }

    // RefcountInc elided this store's INCREMENT as a redundant self-copy (Tier 2):
    // inc(new) and dec(old==new) cancel, so the matching DECREMENT must be elided too --
    // dropping only one side would leak (kept dec) or prematurely free (kept inc). Trust
    // RefcountInc's tag; it is the sole decider and we never recompute the predicate.
    if (I.getMetadata(alaska::kRefcountElidedMD)) {
      return;
    }

    // Only instrument stores into *heap* memory. A store into a stack slot must
    // not be dec-instrumented: refcounting tracks heap->heap references, while
    // stack/register roots are accounted for separately by the conservative stack
    // scan (see runtime/rt/barrier.cpp). Critically, the dec barrier reads the
    // slot's *old* value (load before store) and hands it to alaska_dec_refcount;
    // for the first store to a fresh alloca that load returns uninitialized stack
    // garbage, which -- if its top bit happens to be set -- the runtime decodes as
    // a bogus handle and faults on. underlyingThroughTranslate sees through
    // alaska_translate() (and GEPs/bitcasts), so this also subsumes the old
    // localPinSet special-case.
    auto *underlying = underlyingThroughTranslate(pointerOperand);
    if (isa<AllocaInst>(underlying)) {
      return;
    }
    // An sret return slot is a caller-stack temporary the callee only sees as a
    // hidden pointer argument, so getUnderlyingObject can't reach an alloca. It is
    // likewise uninitialized on entry -- a constructor building the return value
    // into it (e.g. NRVO of a returned pvector: `start_ = new T[n]`) does an
    // *initializing* store with no prior handle. Reading that uninitialized slot is
    // the same uninitialized-garbage fault as above (observed in GAP `tc`), so skip
    // it too. As with stack allocas, such a return slot is really a stack temporary
    // in the caller and is not refcount-tracked.
    if (auto *arg = dyn_cast<Argument>(underlying)) {
      if (arg->hasStructRetAttr()) {
        return;
      }
    }

    // Elide the barrier when the overwritten slot is provably still the null left
    // by a zeroing allocator (first store into fresh halloc/hcalloc memory): the
    // dec would load a guaranteed-null old value and no-op, so skip the load+call
    // entirely. Conservative -- only fires when proven safe (see the helper).
    if (decOverwritesFreshNull(&I, DL, DT, LI)) {
      return;
    }

    // Mark this store for instrumentation - we need to decrement
    // the refcount of whatever value is currently at the pointer location
    toInstrument.push_back({&I, pointerOperand});
  }

  void instrument() {
    if (toInstrument.empty()) return;

    // Get or insert the runtime function: void alaska_dec_refcount(void *handle)
    auto &ctx = M->getContext();
    auto voidTy = Type::getVoidTy(ctx);
    auto ptrTy = PointerType::getUnqual(ctx);
    auto decRefcountType = FunctionType::get(voidTy, {ptrTy}, false);
    auto decRefcountFunc = M->getOrInsertFunction("alaska_dec_refcount", decRefcountType);

    for (auto [storeInst, pointer] : toInstrument) {
      IRBuilder<> builder(storeInst);

      // Load the old value from the pointer
      auto oldValue = builder.CreateLoad(storeInst->getValueOperand()->getType(), pointer, "old_handle");

      // Insert a call to alaska_dec_refcount before the store
      builder.CreateCall(decRefcountFunc, {oldValue});
    }
  }
};

llvm::PreservedAnalyses RefcountDecPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &AM) {
  const llvm::DataLayout &DL = M.getDataLayout();
  for (auto &F : M) {
    if (F.empty()) continue;

    // Skip Alaska runtime functions
    auto section = F.getSection();
    if (section.starts_with("$__ALASKA__")) {
      continue;
    }

    // Per-function analyses for the first-store-into-fresh-allocation elision.
    llvm::DominatorTree DT(F);
    llvm::LoopInfo LI(DT);

    RefcountDecVisitor visitor(&M, DL, DT, LI);
    visitor.visit(F);
    visitor.instrument();
  }

  return PreservedAnalyses::none();
}
