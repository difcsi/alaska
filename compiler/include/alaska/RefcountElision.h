#pragma once

// Shared helpers for the reference-count barrier passes (RefcountInc / RefcountDec).
// These passes run late in alaska-transform (after pointer->handle translate, GC
// statepoint insertion, and localPinSet pinning), so a store's pointer/value is
// typically wrapped in alaska_translate() and constant-offset GEPs/bitcasts.

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace alaska {

// Metadata kind attached by RefcountInc to a store whose INCREMENT barrier it elided
// as a redundant self-copy (Tier 2). RefcountDec elides the matching DECREMENT barrier
// iff this tag is present. RefcountInc is the sole decider/writer; RefcountDec only
// reads it and never recomputes the predicate -- so the two passes cannot disagree and
// asymmetric elision (which would leak or prematurely free) is impossible.
static constexpr const char *kRefcountElidedMD = "alaska.refcount.elided";

// Underlying object of `p`, looking through alaska_translate(). Plain
// getUnderlyingObject stops at that opaque call and never reaches the alloca / sret
// arg / global underneath; strip the translate wrapper(s) so callers see the real
// backing object.
inline llvm::Value *underlyingThroughTranslate(llvm::Value *p) {
  using namespace llvm;
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

// Like underlyingThroughTranslate, but also accumulates the *constant byte offset* from
// the base (so two stores to the same field can be recognized as the same slot). Strips
// constant-offset GEPs/bitcasts and alaska_translate wrappers in a loop; stops at the
// first thing it cannot see through (e.g. a variable-index GEP), returning that value as
// the base.
inline llvm::Value *baseAndConstOffset(llvm::Value *p, const llvm::DataLayout &DL,
                                       llvm::APInt &off) {
  using namespace llvm;
  Value *cur = p;
  while (true) {
    cur = cur->stripAndAccumulateConstantOffsets(DL, off, /*AllowNonInbounds=*/true);
    auto *call = dyn_cast<CallInst>(cur);
    if (call && call->getCalledFunction() &&
        call->getCalledFunction()->getName() == "alaska_translate" &&
        call->arg_size() == 1) {
      cur = call->getArgOperand(0);
      continue;
    }
    return cur;
  }
}

// True if `cb` is alaska_translate(x) -- a pure address derivation that produces no new
// heap reference and does not store to the slot we care about.
inline bool isAlaskaTranslate(const llvm::CallBase *cb) {
  auto *f = cb->getCalledFunction();
  return f && f->getName() == "alaska_translate" && cb->arg_size() == 1;
}

// Tier 2: is store `S` a redundant self-copy `*P = *P` -- storing back into a slot the
// very value just loaded from it, with nothing in between that could change the slot?
// If so, the increment of the new value and the decrement of the (identical) old value
// cancel, so BOTH barriers can be elided.
//
// SOUND and conservative (a wrong "yes" drops a net decrement -> premature free, or a
// net increment -> leak). We restrict to the case that is locally provable:
//   (1) the stored value is a non-volatile load L;
//   (2) L and S are in the SAME basic block, L before S -- the "window" between them is
//       then exactly the straight-line instructions, with no control flow or loop
//       back-edge to reason about (cross-block self-copies are left instrumented);
//   (3) L and S address the IDENTICAL slot: their pointers reduce (through
//       alaska_translate + constant offsets) to the same base SSA value and the same
//       constant offset, so they compute the same address; and
//   (4) nothing in the window writes that slot or anything that might alias it: every
//       instruction between L and S is a pure/read-only op, an alaska_translate, a
//       harmless intrinsic, or a store provably to a DIFFERENT constant slot of the same
//       base. Any other call (could write through an aliasing pointer), any
//       not-provably-disjoint store, or any other memory write disqualifies it.
inline bool storeIsRedundantSelfCopy(llvm::StoreInst *S, const llvm::DataLayout &DL) {
  using namespace llvm;

  // (1) value being stored is a non-volatile load.
  auto *L = dyn_cast<LoadInst>(S->getValueOperand()->stripPointerCasts());
  if (!L) return false;
  if (L->isVolatile() || S->isVolatile()) return false;

  // (2) same basic block, L before S.
  if (L->getParent() != S->getParent()) return false;

  // (3) identical slot: same base SSA value AND same accumulated constant offset.
  unsigned sbits = DL.getIndexTypeSizeInBits(S->getPointerOperand()->getType());
  unsigned lbits = DL.getIndexTypeSizeInBits(L->getPointerOperand()->getType());
  APInt soff(sbits, 0), loff(lbits, 0);
  Value *sbase = baseAndConstOffset(S->getPointerOperand(), DL, soff);
  Value *lbase = baseAndConstOffset(L->getPointerOperand(), DL, loff);
  if (sbase != lbase) return false;
  if (sbits != lbits || soff != loff) return false;

  // (4) clean window: walk straight-line from L to S.
  for (Instruction *I = L->getNextNode(); I && I != S; I = I->getNextNode()) {
    if (auto *cb = dyn_cast<CallBase>(I)) {
      if (isAlaskaTranslate(cb)) continue;
      if (auto *ii = dyn_cast<IntrinsicInst>(I)) {
        Intrinsic::ID id = ii->getIntrinsicID();
        if (id == Intrinsic::lifetime_start || id == Intrinsic::lifetime_end ||
            id == Intrinsic::dbg_value || id == Intrinsic::dbg_declare ||
            id == Intrinsic::dbg_label)
          continue;
      }
      return false;  // any other call might write the slot through an aliasing pointer.
    }
    if (auto *st = dyn_cast<StoreInst>(I)) {
      APInt o(DL.getIndexTypeSizeInBits(st->getPointerOperand()->getType()), 0);
      Value *b = baseAndConstOffset(st->getPointerOperand(), DL, o);
      // Safe only if provably a DIFFERENT constant slot of the same base.
      if (b == sbase && o.getBitWidth() == soff.getBitWidth() && o != soff) continue;
      return false;  // our slot, or an unknown / possibly-aliasing target.
    }
    if (I->mayWriteToMemory()) return false;  // atomicrmw / cmpxchg / fence / volatile.
  }
  return true;
}

}  // namespace alaska
