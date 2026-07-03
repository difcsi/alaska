#pragma once
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"




/**
 * AlaskaTranslatePass - As the core pass in alaska, this pass finds all potential
 * pointers in the program that are victims of load/store operations and ensures
 * that they are first translated if they need to be.
 *
 * It achieves this by walking up the use-def chains of load/store operands to find
 * the 'root allocations' in a function. It then inserts translate runtime calls in
 * a hoisted manner (as close to 'allocation site' as possible, outside of loops)
 * and inserts release runtime calls when those allocations 'die' in a liveness
 * analysis.
 */
class AlaskaTranslatePass : public llvm::PassInfoMixin<AlaskaTranslatePass> {
 public:
   bool hoist = true;
   AlaskaTranslatePass(bool hoist) : hoist(hoist) {}

  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


/**
 * PinTrackingPass - Insert pin roots on the stack so the runtime knows where
 * all active handles are. In a runtime that is able to move handles, it must be
 * aware of all the handles that *cannot* be moved as they are currently in use.
 *
 * This allows each thread to track their active handles *privately* without
 * communicating with other threads until they need to.
 */
class PinTrackingPass : public llvm::PassInfoMixin<PinTrackingPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


// This is implemented in PinTracking.cpp, as it uses many of the same helper functions.
class HandleFaultPass : public llvm::PassInfoMixin<HandleFaultPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


/**
 * TranslationPrinterPass - A pass which extracts translation information of each
 * function and prints them to stdout in a .dot format.
 */
class TranslationPrinterPass : public llvm::PassInfoMixin<TranslationPrinterPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


class RedundantArgumentPinElisionPass
    : public llvm::PassInfoMixin<RedundantArgumentPinElisionPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


/**
 * AlaskaNormalizePass - LLVM IR is complicated and has certain constructs that
 * make analysis difficult sometimes. This pass is simple and just lowers many
 * of those constructs into forms that are more easily analyzable.
 */
class AlaskaNormalizePass : public llvm::PassInfoMixin<AlaskaNormalizePass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

class AlaskaEscapePass : public llvm::PassInfoMixin<AlaskaEscapePass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


/**
 * AlaskaHoistInductionTranslatePass - Strength-reduce translations of loop
 * pointer-induction variables into "translated space". AlaskaTranslatePass
 * anchors a translation at the induction phi (phis are unconditional roots),
 * so a pointer walked through a loop is re-translated every iteration. This
 * pass hoists the translate of the loop-invariant base into the preheader and
 * advances a translated induction pointer in the loop, removing the per-edge
 * translate. See HoistInductionTranslate.cpp for the pinning argument. Opt-in;
 * must run after `alaska-translate` and before `alaska-tracking`.
 */
class AlaskaHoistInductionTranslatePass
    : public llvm::PassInfoMixin<AlaskaHoistInductionTranslatePass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


/**
 * AlaskaStackPromotePass ("Yukon") - Promote address-taken stack objects to the
 * heap so they become handles. A pycallocs/Python proxy can only be built for a
 * heap object (Python may relocate it, e.g. on list.append), so any stack
 * pointer that may escape into Python must first live behind a handle.
 *
 * Must run before AlaskaTranslatePass: an `alloca` is deliberately never
 * translated, but the `halloc` call it is rewritten into is treated as a
 * translation root, routing the object's loads/stores through the handle
 * machinery automatically.
 *
 * Modes are selected at run time via environment variables:
 *   ALASKA_HEAP_HINTS=<file>  - promote only allocas whose address flows into a
 *                               listed function's pointer argument (LinkPy emits
 *                               this list). When unset, every address-taken
 *                               stack object is promoted.
 *   ALASKA_STACK_PROMOTE_FREE=0|1 - override whether promoted objects are
 *                               hfree'd at function return.
 */
class AlaskaStackPromotePass : public llvm::PassInfoMixin<AlaskaStackPromotePass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

class AlaskaReplacementPass : public llvm::PassInfoMixin<AlaskaReplacementPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


class AlaskaLowerPass : public llvm::PassInfoMixin<AlaskaLowerPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};



class AlaskaArgumentTracePass : public llvm::PassInfoMixin<AlaskaArgumentTracePass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};


#if ALASKA_ENABLE_REFCOUNT
/**
 * RefcountIncPass - Increment refcount when a handle is written to memory.
 * This pass instruments stores of pointer values to insert calls to
 * alaska_inc_refcount, ensuring proper reference counting for handles.
 */
class RefcountIncPass : public llvm::PassInfoMixin<RefcountIncPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

/**
 * RefcountDecPass - Decrement refcount when a handle is overwritten.
 * This pass instruments stores of pointer values to insert calls to
 * alaska_dec_refcount for the old value being replaced, ensuring proper
 * reference counting when handles are overwritten.
 */
class RefcountDecPass : public llvm::PassInfoMixin<RefcountDecPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};
#endif  // ALASKA_ENABLE_REFCOUNT
