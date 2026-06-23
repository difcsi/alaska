#include <alaska/Passes.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;

// Underlying object of `p`, looking through alaska_translate(). The refcount
// passes run *after* alaska-translate (see bin/alaska-transform), so a store's
// pointer is typically `alaska_translate(X)` (possibly behind GEPs/bitcasts).
// Plain getUnderlyingObject stops at that opaque call and never reaches the
// alloca / sret arg underneath, defeating the stack/sret skips below. Strip the
// translate wrapper(s) so those skips see the real backing object.
static Value *underlyingThroughTranslate(Value *p) {
  Value *u = getUnderlyingObject(p);
  while (auto *call = dyn_cast<CallInst>(u)) {
    auto *callee = call->getCalledFunction();
    if (callee && callee->getName() == "alaska_translate" && call->arg_size() == 1) {
      u = getUnderlyingObject(call->getArgOperand(0));
    } else {
      break;
    }
  }
  return u;
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
  std::vector<std::pair<StoreInst *, Value *>> toInstrument;

  RefcountDecVisitor(llvm::Module *M) : M(M) {}

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
  for (auto &F : M) {
    if (F.empty()) continue;
    
    // Skip Alaska runtime functions
    auto section = F.getSection();
    if (section.starts_with("$__ALASKA__")) {
      continue;
    }

    RefcountDecVisitor visitor(&M);
    visitor.visit(F);
    visitor.instrument();
  }

  return PreservedAnalyses::none();
}
