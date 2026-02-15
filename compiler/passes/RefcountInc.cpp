#include <alaska/Passes.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;

/**
 * RefcountIncPass - Inserts calls to increment the refcount when a handle
 * is written to memory. This ensures that handles that are stored maintain
 * proper reference counts for lifetime tracking.
 */

class RefcountIncVisitor : public llvm::InstVisitor<RefcountIncVisitor> {
 public:
  llvm::Module *M;
  std::vector<std::pair<StoreInst *, Value *>> toInstrument;

  RefcountIncVisitor(llvm::Module *M) : M(M) {}

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

    // Skip stores to localPinSet - this is compiler bookkeeping for GC tracking
    // and shouldn't affect user-visible refcounts
    if (auto *gep = dyn_cast<GetElementPtrInst>(pointerOperand)) {
      if (auto *alloca = dyn_cast<AllocaInst>(gep->getPointerOperand())) {
        if (alloca->getName() == "localPinSet") {
          return;
        }
      }
    }

    // Mark this store for instrumentation
    toInstrument.push_back({&I, valueOperand});
  }

  void instrument() {
    if (toInstrument.empty()) return;

    // Get or insert the runtime function: void alaska_inc_refcount(void *handle)
    auto &ctx = M->getContext();
    auto voidTy = Type::getVoidTy(ctx);
    auto ptrTy = PointerType::getUnqual(ctx);
    auto incRefcountType = FunctionType::get(voidTy, {ptrTy}, false);
    auto incRefcountFunc = M->getOrInsertFunction("alaska_inc_refcount", incRefcountType);

    for (auto [storeInst, value] : toInstrument) {
      IRBuilder<> builder(storeInst);
      
      // Insert a call to alaska_inc_refcount before the store
      builder.CreateCall(incRefcountFunc, {value});
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
