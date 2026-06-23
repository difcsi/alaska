#include <alaska/Passes.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/IntrinsicInst.h>
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
 * RefcountIncPass - Inserts calls to increment the refcount when a handle
 * is written to memory. This ensures that handles that are stored maintain
 * proper reference counts for lifetime tracking.
 */

class RefcountIncVisitor : public llvm::InstVisitor<RefcountIncVisitor> {
 public:
  llvm::Module *M;
  std::vector<std::pair<StoreInst *, Value *>> toInstrument;
  // Byte copies to instrument: {copy instruction, {dest pointer, length}}. A
  // memcpy/memmove of handle-containing memory deposits new heap references that
  // the pointer-typed store barrier never sees, so we scan the destination range
  // at runtime and inc each handle (see alaska_inc_handles_in_range).
  std::vector<std::pair<Instruction *, std::pair<Value *, Value *>>> copiesToInstrument;

  RefcountIncVisitor(llvm::Module *M) : M(M) {}

  // Record a byte copy for instrumentation unless its destination is a stack
  // slot / sret temporary -- mirroring the heap-only rule for stores, since the
  // matching dec only ever runs on heap objects (alaska_hfree_dec_children).
  void recordCopy(Instruction &I, Value *dest, Value *len) {
    auto *underlying = underlyingThroughTranslate(dest);
    if (isa<AllocaInst>(underlying)) return;
    if (auto *arg = dyn_cast<Argument>(underlying)) {
      if (arg->hasStructRetAttr()) return;
    }
    copiesToInstrument.push_back({&I, {dest, len}});
  }

  // llvm.memcpy / llvm.memmove intrinsics (what clang emits for most struct and
  // container copies).
  void visitMemTransferInst(llvm::MemTransferInst &I) {
    recordCopy(I, I.getRawDest(), I.getLength());
  }

  // Explicit calls to the libc memcpy / memmove symbols.
  void visitCallInst(llvm::CallInst &I) {
    auto *callee = I.getCalledFunction();
    if (callee == nullptr) return;
    auto name = callee->getName();
    if ((name == "memcpy" || name == "memmove") && I.arg_size() >= 3) {
      recordCopy(I, I.getArgOperand(0), I.getArgOperand(2));
    }
  }

  void visitStoreInst(llvm::StoreInst &I) {
    auto valueOperand = I.getValueOperand();
    auto pointerOperand = I.getPointerOperand();

    // Check if we're storing a pointer type (potential handle)
    if (!valueOperand->getType()->isPointerTy()) {
      return;
    }

    // Check if the pointer operand is also a pointer (handle to handle store)
    if (!pointerOperand->getType()->isPointerTy()) {
     // return;
    }

    // Only instrument stores into *heap* memory, mirroring RefcountDec: a handle
    // stored into a stack slot is a new root, kept alive by the conservative stack
    // scan (runtime/rt/barrier.cpp), not by a heap refcount. Were we to inc here
    // while RefcountDec (correctly) skips the matching stack store, the handle's
    // refcount would ratchet up with no balancing dec and never be reclaimed.
    // underlyingThroughTranslate sees through alaska_translate() (and
    // GEPs/bitcasts) and subsumes the old localPinSet special-case.
    auto *underlying = underlyingThroughTranslate(pointerOperand);
    if (isa<AllocaInst>(underlying)) {
      return;
    }
    // An sret return slot is a caller-stack temporary (see RefcountDec for the
    // full rationale). Skip it here too, so inc/dec stay balanced: RefcountDec
    // skips the matching store, and an inc with no balancing dec would leak the
    // refcount and prevent reclamation.
    if (auto *arg = dyn_cast<Argument>(underlying)) {
      if (arg->hasStructRetAttr()) {
        return;
      }
    }

    // Mark this store for instrumentation
    toInstrument.push_back({&I, valueOperand});
  }

  void instrument() {
    if (toInstrument.empty() && copiesToInstrument.empty()) return;

    auto &ctx = M->getContext();
    auto voidTy = Type::getVoidTy(ctx);
    auto ptrTy = PointerType::getUnqual(ctx);
    auto i64Ty = Type::getInt64Ty(ctx);

    // void alaska_inc_refcount(void *handle)
    auto incRefcountType = FunctionType::get(voidTy, {ptrTy}, false);
    auto incRefcountFunc = M->getOrInsertFunction("alaska_inc_refcount", incRefcountType);

    for (auto [storeInst, value] : toInstrument) {
      IRBuilder<> builder(storeInst);
      // Insert a call to alaska_inc_refcount before the store
      builder.CreateCall(incRefcountFunc, {value});
    }

    if (!copiesToInstrument.empty()) {
      // void alaska_inc_handles_in_range(void *dest, size_t bytes)
      auto incRangeType = FunctionType::get(voidTy, {ptrTy, i64Ty}, false);
      auto incRangeFunc = M->getOrInsertFunction("alaska_inc_handles_in_range", incRangeType);

      for (auto &[copyInst, destLen] : copiesToInstrument) {
        // Inc the handles in the destination *after* the copy populates it.
        IRBuilder<> builder(copyInst->getNextNode());
        Value *len = destLen.second;
        if (len->getType() != i64Ty) len = builder.CreateZExtOrTrunc(len, i64Ty);
        builder.CreateCall(incRangeFunc, {destLen.first, len});
      }
    }
  }
};

llvm::PreservedAnalyses RefcountIncPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &AM) {
  for (auto &F : M) {
    if (F.empty()) continue;
    
    // Skip Alaska runtime functions
    auto section = F.getSection();
    if (section.starts_with("$__ALASKA__")) {
      continue;
    }

    RefcountIncVisitor visitor(&M);
    visitor.visit(F);
    visitor.instrument();
  }

  return PreservedAnalyses::none();
}
