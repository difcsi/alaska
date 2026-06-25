#include <alaska/EscapeAnalysis.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace alaska {

// True if passing our pointer to call `cb` at argument `argIdx` neither lets it
// escape NOR frees it -- both of which would make keeping the caller's allocation
// raw unsound. (`nocapture` alone is not enough: `free` itself is nocapture, so a
// nocapture callee may still free the argument, and that free -- rewritten to
// hfree module-wide and unseen in the caller -- would leak a raw pointer.)
//
// Fast path: a call that does not capture the pointer AND only reads memory
// cannot keep a copy of it and cannot free it (freeing writes). Sound even for
// external declarations.
//
// Otherwise, one level of interprocedural analysis: a defined, non-vararg callee
// whose parameter does not escape its own body. The nested analysis runs with
// allocatorCallsAreUses=false (so a free/realloc of the parameter inside the
// callee counts as an escape, excluding the leak case) and checkNoCapture=false
// (a parameter passed onward to another call is conservatively an escape -- we
// reason one level deep only).
static bool callDoesNotLeakArg(CallBase *cb, unsigned argIdx) {
  if (cb->doesNotCapture(argIdx) && cb->onlyReadsMemory()) return true;

  auto *callee = dyn_cast<Function>(cb->getCalledOperand()->stripPointerCasts());
  if (!callee || callee->isDeclaration() || callee->isVarArg()) return false;
  if (argIdx >= callee->arg_size()) return false;

  Argument *param = callee->getArg(argIdx);
  if (!param->getType()->isPointerTy()) return false;

  EscapeInfo sub = analyzeEscape(param, /*hints=*/nullptr, /*allocatorCallsAreUses=*/false,
      /*followReallocChain=*/false, /*checkNoCapture=*/false);
  return !sub.taken && !sub.returned && !sub.unsafe;
}

EscapeInfo analyzeEscape(Value *root, const std::set<std::string> *hints,
    bool allocatorCallsAreUses, bool followReallocChain, bool checkNoCapture) {
  EscapeInfo info;
  info.regionProducers.insert(root);

  SmallVector<Value *, 16> work;
  SmallPtrSet<Value *, 16> seen;
  work.push_back(root);
  seen.insert(root);

  while (!work.empty()) {
    Value *V = work.pop_back_val();
    for (Use &U : V->uses()) {
      auto *I = dyn_cast<Instruction>(U.getUser());
      if (!I) {
        // A constant expression (e.g. ptrtoint in a global initializer) takes
        // the address out of our sight; be conservative.
        info.taken = true;
        continue;
      }

      // Reading/writing *through* the pointer is a use of the object, not an
      // escape of its address. Storing the address itself, however, escapes.
      if (auto *li = dyn_cast<LoadInst>(I)) {
        if (li->getPointerOperand() == V) continue;
        info.taken = true;
        continue;
      }
      if (auto *si = dyn_cast<StoreInst>(I)) {
        if (si->getValueOperand() == V) {  // the address is the value stored
          info.taken = true;
          continue;
        }
        if (si->getPointerOperand() == V) continue;  // storing into the object
        info.taken = true;
        continue;
      }

      // Transient pointer producers: keep walking.
      if (isa<BitCastInst>(I) || isa<GetElementPtrInst>(I) ||
          isa<AddrSpaceCastInst>(I) || isa<PHINode>(I) || isa<SelectInst>(I)) {
        if (seen.insert(I).second) work.push_back(I);
        continue;
      }

      if (auto *ii = dyn_cast<IntrinsicInst>(I)) {
        switch (ii->getIntrinsicID()) {
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
            info.lifetimeMarkers.insert(ii);
            continue;
          case Intrinsic::dbg_declare:
          case Intrinsic::dbg_value:
          case Intrinsic::dbg_label:
          case Intrinsic::memcpy:
          case Intrinsic::memmove:
          case Intrinsic::memset:
            continue;  // accesses / debug info, not escapes
          case Intrinsic::vastart:
          case Intrinsic::vacopy:
          case Intrinsic::vaend:
            info.unsafe = true;  // va_list slot: ABI-sensitive, never touch
            continue;
          default:
            info.taken = true;  // unknown intrinsic taking the address
            continue;
        }
      }

      // Atomic accesses are not handled by the translate pass (only plain
      // load/store are sinks), so an object accessed atomically must not be
      // promoted (StackPromote) nor kept-raw-with-translation-suppressed
      // (Replacement). Mark it unsafe either way.
      if (auto *rmw = dyn_cast<AtomicRMWInst>(I)) {
        if (rmw->getPointerOperand() == V) info.unsafe = true;
        else info.taken = true;
        continue;
      }
      if (auto *cx = dyn_cast<AtomicCmpXchgInst>(I)) {
        if (cx->getPointerOperand() == V) info.unsafe = true;
        else info.taken = true;
        continue;
      }

      if (auto *cb = dyn_cast<CallBase>(I)) {
        // Inline asm (and asm-goto) may rely on the operand being a real address;
        // handing it a handle would corrupt the access. Never touch.
        if (cb->isInlineAsm()) {
          info.unsafe = true;
          continue;
        }

        // At which argument position (if any) is our pointer passed? (-1 if our
        // pointer is the called operand -- an indirect call *through* it, which
        // is an escape handled by the `taken` fall-through below.)
        int argIdx = -1;
        for (unsigned a = 0, n = cb->arg_size(); a < n; a++) {
          if (cb->getArgOperand(a) == V) {
            argIdx = (int)a;
            break;
          }
        }

        // Allocator-aware mode: a pointer consumed by a dealloc/size-query or
        // realloc routine at its pointer slot (arg 0) is not an escape.
        if (allocatorCallsAreUses && argIdx == 0) {
          if (auto *callee = dyn_cast<Function>(cb->getCalledOperand()->stripPointerCasts())) {
            StringRef name = callee->getName();
            if (name == "free" || name == "hfree" || name == "malloc_usable_size" ||
                name == "alaska_usable_size") {
              info.deallocUses.insert(cb);
              continue;
            }
            if ((name == "realloc" || name == "hrealloc") && followReallocChain) {
              // The realloc result is a continuation of the same logical
              // allocation: record the call and follow its result. (Without
              // followReallocChain, realloc falls through to the escape handling
              // below.)
              info.reallocUses.insert(cb);
              info.regionProducers.insert(cb);
              if (seen.insert(cb).second) work.push_back(cb);
              continue;
            }
          }
        }

        // Case (b): the call neither captures nor frees this argument, so the
        // pointer does not escape through it.
        if (checkNoCapture && argIdx >= 0 && callDoesNotLeakArg(cb, (unsigned)argIdx)) {
          continue;
        }

        info.taken = true;
        if (argIdx >= 0 && hints) {
          if (auto *callee =
                  dyn_cast<Function>(cb->getCalledOperand()->stripPointerCasts())) {
            if (hints->count(std::string(callee->getName()))) info.toHinted = true;
          }
        }
        continue;
      }

      if (isa<ReturnInst>(I)) {
        info.taken = true;
        info.returned = true;
        continue;
      }

      // ptrtoint / icmp and anything else: the address has escaped our view.
      info.taken = true;
    }
  }
  return info;
}

}  // namespace alaska
