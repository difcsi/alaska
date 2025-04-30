
#include <alaska/Passes.h>
#include <llvm/IR/PassManager.h>
#include "alaska/AccessAutomata.h"
#include "alaska/OptimisticTypes.h"
#include "alaska/Translations.h"
#include <alaska/Utils.h>


llvm::Function *constructTrace(llvm::Use &use) {
  // start at this use, walk the CFG, until you either reach the end of the function
  // or this use again, and construct a function which is the trace.


  alaska::println("making trace for ", use);
  auto *inst = llvm::cast<llvm::Instruction>(use.getUser());
  auto *ptr = inst;  // this is the call to alaska.translate()

  return NULL;

  while (true) {
    inst = inst->getNextNode();
    if (inst == nullptr) {
      break;
    }

    if (auto load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      //
    }

    if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
      //
    }
  }


  return NULL;
}

llvm::PreservedAnalyses AlaskaIntentPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &AM) {
  for (auto &F : M) {
    if (F.getName() == "search") {
      auto translations = alaska::extractTranslations(F);
      for (auto &tr : translations) {
        //
        constructTrace(tr->translation->getArgOperandUse(0));
      }
      alaska::printTranslationDot(F, translations);
    }
  }
  return llvm::PreservedAnalyses::none();
}
