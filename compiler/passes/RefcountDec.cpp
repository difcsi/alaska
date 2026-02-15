#include <alaska/Passes.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;

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

    // Skip stores to localPinSet - this is compiler bookkeeping for GC tracking
    // and shouldn't affect user-visible refcounts
    if (auto *gep = dyn_cast<GetElementPtrInst>(pointerOperand)) {
      if (auto *alloca = dyn_cast<AllocaInst>(gep->getPointerOperand())) {
        if (alloca->getName() == "localPinSet") {
          return;
        }
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
