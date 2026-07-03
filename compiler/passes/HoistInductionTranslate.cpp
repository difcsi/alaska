// HoistInductionTranslate.cpp - strength-reduce translations of loop-induction
// pointers into translated space.
//
// Motivation (GAP pr / pr_spmv, SpMV-style kernels). The hot loop
//
//     for (NodeID v : g.in_neigh(u))   //  cursor walks the CSR adjacency array
//       sum += contrib[v];
//
// compiles to a pointer-induction phi over the neighbour list:
//
//     %p = phi ptr [ %begin, %ph ], [ %next, %latch ]
//     %v = load i32, ptr alaska.translate(%p)         ; <-- translate EVERY edge
//     ...
//     %next = getelementptr i32, ptr %p, 1
//
// AlaskaTranslatePass anchors the translation at the phi (phis are unconditional
// translation roots, to break back-edge cycles in the pointer-flow graph), and
// its hoisting heuristic only lifts pointers defined *outside* the loop. So the
// `alaska.translate` call stays in the loop body and runs once per edge, even
// though the *object* it derives from (the neighbour array) is loop-invariant.
// For a kernel that does ~one add of real work per edge, that per-edge translate
// (plus the back-edge safepoint PlaceSafepoints later inserts) dominates.
//
// This pass recognises the affine induction and re-expresses the walk in
// translated space:
//
//     %tbase = alaska.translate(%begin)                ; hoisted, once per entry
//     %tp = phi ptr [ %tbase, %ph ], [ %tnext, %latch ]
//     %v  = load i32, ptr %tp                          ; no per-edge translate
//     %tnext = getelementptr i32, ptr %tp, 1
//
// Correctness rests on translation being affine within an object:
//   translate(p + k) == translate(p) + k
// so advancing the translated pointer by the same element offset tracks
// translate(p) at every iteration.
//
// Pinning across the (later-inserted) back-edge safepoint: `alaska.release`
// markers bound a translation's live range, and PinTracking pins every
// translation live across a statepoint via the localPinSet. The per-edge
// translate is live for only one iteration; the hoisted one is live across the
// whole loop, so we emit an `alaska.release` on each loop-exit edge (mirroring
// TranslationForest's own SplitEdge + release idiom). That makes the object pin
// for the loop's duration -- exactly what the currently-hoisted `contrib` base
// already does. This pass therefore MUST run after `alaska-translate` and before
// `alaska-tracking` (PlaceSafepoints + PinTracking), while translations are
// still `alaska.translate`/`alaska.root` markers.
//
// The transform is deliberately opt-in (driver flag `--hoist-induction`); it is
// inert unless requested, so default pipelines are unchanged.

#include <alaska/Passes.h>
#include <alaska/Utils.h>
#include <alaska/Translations.h>

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

using namespace llvm;

static bool isNamedCall(Value *v, StringRef name) {
  if (auto *ci = dyn_cast<CallInst>(v))
    if (auto *f = ci->getCalledFunction()) return f->getName() == name;
  return false;
}

// alaska.root(x) -> x, else identity.
static Value *lookThroughRoot(Value *v) {
  if (isNamedCall(v, "alaska.root")) return cast<CallInst>(v)->getArgOperand(0);
  return v;
}

llvm::PreservedAnalyses AlaskaHoistInductionTranslatePass::run(
    Module &M, ModuleAnalysisManager &AM) {
  if (getenv("ALASKA_DISABLE_INDUCTION_HOIST")) return PreservedAnalyses::all();

  auto *translateFn = M.getFunction("alaska.translate");
  if (translateFn == nullptr) return PreservedAnalyses::all();

  bool changed = false;

  for (auto &F : M) {
    if (F.empty()) continue;

    DominatorTree DT(F);
    LoopInfo LI(DT);
    if (LI.empty()) continue;

    // Group translation markers by the induction phi they translate. A phi may
    // be dereferenced through more than one translate call; all of them can be
    // redirected to the single translated-space induction.
    MapVector<PHINode *, SmallVector<CallInst *, 2>> candidates;
    for (auto &I : instructions(F)) {
      auto *call = dyn_cast<CallInst>(&I);
      if (!call || call->getCalledFunction() != translateFn) continue;
      if (auto *phi = dyn_cast<PHINode>(lookThroughRoot(call->getArgOperand(0))))
        candidates[phi].push_back(call);
    }

    for (auto &[phi, calls] : candidates) {
      // The phi must be the induction variable of a simplified loop: it lives in
      // the header and has exactly {invariant base from the preheader, self-GEP
      // from the latch}.
      Loop *L = LI.getLoopFor(phi->getParent());
      if (!L || phi->getParent() != L->getHeader()) continue;

      BasicBlock *ph = L->getLoopPreheader();
      BasicBlock *latch = L->getLoopLatch();
      if (!ph || !latch) continue;
      if (phi->getNumIncomingValues() != 2) continue;

      Value *base = phi->getIncomingValueForBlock(ph);
      Value *stepV = phi->getIncomingValueForBlock(latch);
      if (!base || !stepV || !base->getType()->isPointerTy()) continue;
      if (!L->isLoopInvariant(base)) continue;

      // The latch value must be a GEP that advances the phi by a loop-invariant
      // (affine) offset.
      auto *gep = dyn_cast<GetElementPtrInst>(stepV);
      if (!gep || gep->getPointerOperand() != phi) continue;
      bool affine = true;
      for (auto &idx : gep->indices())
        if (!L->isLoopInvariant(idx.get())) {
          affine = false;
          break;
        }
      if (!affine) continue;

      // --- restrict to the intended pattern: a streaming scan over a flat array
      // of *scalar* elements behind a *real handle* ---
      // Without these gates the pass also rewrites STL-container iterations
      // (std::sort's introsort over std::pair, basic_string / vector walks, etc.)
      // -- hundreds of loops per translation unit, none of which are the CSR
      // adjacency-scan target and all of which enlarge the risk surface. The
      // graph kernels iterate i32 (neighbour ids), float/double (scores) and ptr
      // arrays, so gating on a scalar element type keeps every intended win while
      // excluding aggregate container internals.
      llvm::Type *eltTy = gep->getSourceElementType();
      bool scalarOK = eltTy->isIntegerTy() || eltTy->isFloatingPointTy() || eltTy->isPointerTy();
      // Only strength-reduce a base the forest itself would treat as a handle
      // (excludes globals, allocas, operator-new[] results and keep-raw libc
      // pointers -- translating those is never what we want).
      bool handleOK = alaska::shouldTranslate(base);
      if (getenv("ALASKA_HOIST_DEBUG")) {
        const char *reason = (scalarOK && handleOK) ? "ACCEPT"
            : (!scalarOK ? "SKIP-aggregate-elt" : "SKIP-nonhandle-base");
        llvm::errs() << "[hoist-ind] " << F.getName() << " " << reason << " elt=";
        eltTy->print(llvm::errs());
        llvm::errs() << " base=[";
        if (auto *bi = dyn_cast<Instruction>(base)) llvm::errs() << bi->getOpcodeName();
        else llvm::errs() << "non-inst";
        llvm::errs() << "] ";
        base->printAsOperand(llvm::errs(), false);
        llvm::errs() << "\n";
      }
      if (!scalarOK || !handleOK) continue;

      // --- translate the invariant base once, in the preheader ---
      Instruction *term = ph->getTerminator();
      Instruction *rbase = alaska::insertRootBefore(term, base);
      Instruction *tbase = alaska::insertTranslationBefore(term, rbase);

      // --- parallel induction in translated space ---
      PHINode *tphi = PHINode::Create(phi->getType(), 2, phi->getName() + ".xlat", phi);
      SmallVector<Value *, 4> idxs(gep->idx_begin(), gep->idx_end());
      IRBuilder<> gb(gep->getNextNode());  // immediately after the handle-space step
      Value *tnext = gb.CreateGEP(
          gep->getSourceElementType(), tphi, idxs, gep->getName() + ".xlat", gep->isInBounds());
      tphi->addIncoming(tbase, ph);
      tphi->addIncoming(tnext, latch);

      // --- redirect every dereference of translate(phi) to the streamed ptr ---
      for (auto *call : calls) {
        Value *arg = call->getArgOperand(0);
        call->replaceAllUsesWith(tphi);
        call->eraseFromParent();
        // Drop the now-dead alaska.root(phi) wrapper if nothing else uses it.
        if (auto *root = dyn_cast<CallInst>(arg))
          if (isNamedCall(root, "alaska.root") && root->use_empty()) root->eraseFromParent();
      }

      // --- keep the object pinned for the whole loop ---
      // Release on every exit edge so PinTracking sees the hoisted translation
      // live across the back-edge safepoint. The release operand must be the
      // *root-wrapped* value: Translation::getHandle() returns the translate's
      // first operand verbatim (no alaska.root unwrap), and release matching is
      // getHandle() == release-operand. Releasing `base` instead of `rbase`
      // silently fails to match, collapsing the live range to the def block and
      // leaving the object unpinned across the loop's safepoint.
      SmallVector<Loop::Edge, 4> exitEdges;
      L->getExitEdges(exitEdges);
      for (auto &e : exitEdges) {
        BasicBlock *tramp = SplitEdge(e.first, e.second, &DT, &LI);
        alaska::insertReleaseBefore(tramp->getTerminator(), rbase);
      }

      changed = true;
    }
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
