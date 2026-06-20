// StackPromote.cpp - "Yukon" stack-to-heap promotion.
//
// This pass rewrites address-taken stack objects (allocas whose address
// escapes the function) into heap allocations using Alaska's own `halloc`,
// so that they become first-class *handles*. The motivation is the pycallocs
// / LinkPy integration: a Python proxy can only be built for a heap object,
// because Python may trigger a reallocation through language features such as
// `list.append`. Stack objects cannot move/relocate, so any pointer that may
// escape into Python must first live on the heap behind a handle.
//
// Why this is essentially free in Alaska: `alaska::shouldTranslate` returns
// false for an `alloca` and `PointerFlowGraph::visitAlloca` assigns it no
// color, so allocas are deliberately never translated. A call to `halloc`,
// on the other hand, is treated as a colored allocation source and becomes a
// translation root. Swapping `alloca` -> `call halloc` therefore routes every
// load/store of the object through the handle machinery with no other change,
// provided this pass runs *before* `alaska-translate`.
//
// Two modes, selected by the `ALASKA_HEAP_HINTS` environment variable:
//
//   * promote-all (default): promote every address-taken stack object.
//     This is intentionally over-eager (see the Yukon paper) -- only pointers
//     that escape into Python actually need promotion.
//
//   * heap-hints (ALASKA_HEAP_HINTS=<file>): LinkPy's stub generator knows
//     precisely which functions' pointer arguments may escape into Python and
//     emits their names, one per line. In this mode only allocas whose address
//     flows into one of those functions' pointer arguments are promoted, e.g.
//
//         extern void py_mirrorx(Point*);          // listed in the hints file
//         int foo() {
//           Point a = {2, 3};
//           Point b = {3, 2};
//           printf("%p", (void*)&b);  // b NOT promoted (printf not hinted)
//           py_mirrorx(&a);           // a promoted (py_mirrorx is hinted)
//         }
//
// Lifetime: a promoted object keeps stack semantics by default -- `hfree` is
// inserted at every `ret`. The Python-escape scenario is the exception: a
// proxied object must outlive its C frame, so in heap-hints mode the frees are
// suppressed by default and reclamation is left to the handle/refcount system.
// `ALASKA_STACK_PROMOTE_FREE=0|1` overrides the default either way. An object
// whose address is *returned* is never freed (that free would always be wrong).

#include <alaska/Passes.h>
#include <alaska/Utils.h>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace {

// Read newline-separated function names from `path` into `out`. Blank lines and
// lines beginning with '#' are ignored; surrounding whitespace is trimmed.
// Returns false if the file could not be opened.
static bool loadHintFile(const std::string &path, std::set<std::string> &out) {
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::string line;
  while (std::getline(in, line)) {
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    size_t e = line.find_last_not_of(" \t\r\n");
    std::string name = line.substr(b, e - b + 1);
    if (name.empty() || name[0] == '#') continue;
    out.insert(name);
  }
  return true;
}

// Functions whose bodies must never be touched: declarations, Alaska runtime
// code (which would recurse if it allocated via halloc), and the allocator
// entry points themselves.
static bool skipFunction(const Function &F) {
  if (F.empty()) return true;
  StringRef n = F.getName();
  if (n.startswith("alaska") || n.startswith("__alaska") ||
      n.startswith("anchorage") || n.startswith("yukon") || n.startswith("llvm."))
    return true;
  if (F.getSection().startswith("$__ALASKA__")) return true;
  if (n == "halloc" || n == "hcalloc" || n == "hrealloc" || n == "hfree" ||
      n == "malloc" || n == "calloc" || n == "realloc" || n == "free" ||
      n == "alaska_usable_size")
    return true;
  return false;
}

// Classification of how a stack object's address is used.
struct EscapeInfo {
  bool taken = false;     // address escapes the object in any way
  bool toHinted = false;  // address flows into a hinted function's pointer arg
  bool returned = false;  // address is returned from the function
  bool unsafe = false;    // used in a way we must not promote (atomics, va_list)
  // lifetime/invariant markers on the object; meaningless once it is on the
  // heap, so they are removed at promotion time.
  SmallPtrSet<IntrinsicInst *, 4> lifetimeMarkers;
};

// Walk the def-use chain of `AI`, following pointer-producing transient
// instructions (bitcast/gep/addrspacecast/phi/select), and classify every use.
// When `hints` is non-null, also record whether the address reaches a call to
// one of those functions as a pointer argument.
static EscapeInfo analyzeEscape(AllocaInst *AI, const std::set<std::string> *hints) {
  EscapeInfo info;
  SmallVector<Value *, 16> work;
  SmallPtrSet<Value *, 16> seen;
  work.push_back(AI);
  seen.insert(AI);

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
            info.unsafe = true;  // va_list slot: ABI-sensitive, never promote
            continue;
          default:
            info.taken = true;  // unknown intrinsic taking the address
            continue;
        }
      }

      // Atomic accesses are not handled by the translate pass (only plain
      // load/store are sinks), so promoting an object accessed atomically would
      // dereference a raw handle. Refuse to promote such objects.
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
        // Inline asm (and asm-goto) may rely on the operand being a real stack
        // address; handing it a handle would corrupt the access. Never promote.
        if (cb->isInlineAsm()) {
          info.unsafe = true;
          continue;
        }
        bool isArg = false;
        for (unsigned a = 0, n = cb->arg_size(); a < n; a++) {
          if (cb->getArgOperand(a) == V) {
            isArg = true;
            break;
          }
        }
        info.taken = true;
        if (isArg && hints) {
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

// Decide whether to free promoted objects at function exit, given the mode.
// Default: free in promote-all mode (leak-free stack semantics), do not free in
// heap-hints mode (Python-escaped objects outlive the C frame). Overridable.
static bool freeOnReturnDefault(bool hintsMode) {
  if (const char *e = getenv("ALASKA_STACK_PROMOTE_FREE")) {
    StringRef s(e);
    if (s == "0" || s == "no" || s == "false" || s == "off") return false;
    if (s == "1" || s == "yes" || s == "true" || s == "on") return true;
  }
  return !hintsMode;
}

}  // namespace

PreservedAnalyses AlaskaStackPromotePass::run(Module &M, ModuleAnalysisManager &AM) {
  std::set<std::string> hints;
  bool hintsMode = false;
  if (const char *hintsPath = getenv("ALASKA_HEAP_HINTS")) {
    if (hintsPath[0] != '\0') {
      if (loadHintFile(hintsPath, hints)) {
        hintsMode = true;
        alaska::println("[stack-promote] heap hints: ", hints.size(),
            " function(s) from ", hintsPath);
      } else {
        alaska::println("[stack-promote] WARNING: could not open heap hints file '",
            hintsPath, "'; promoting all address-taken stack objects");
      }
    }
  }

  const bool freeOnReturn = freeOnReturnDefault(hintsMode);

  auto &ctx = M.getContext();
  auto *ptrTy = PointerType::get(ctx, 0);
  auto *i64Ty = Type::getInt64Ty(ctx);
  auto *voidTy = Type::getVoidTy(ctx);
  FunctionCallee hallocFn =
      M.getOrInsertFunction("halloc", FunctionType::get(ptrTy, {i64Ty}, false));
  FunctionCallee hfreeFn =
      M.getOrInsertFunction("hfree", FunctionType::get(voidTy, {ptrTy}, false));

  const DataLayout &DL = M.getDataLayout();
  unsigned long promoted = 0;

  for (auto &F : M) {
    if (skipFunction(F)) continue;

    // Only entry-block allocas are promoted. They execute exactly once per call
    // (so one halloc / one hfree), unlike allocas inside loops which would leak.
    std::vector<AllocaInst *> candidates;
    for (auto &I : F.getEntryBlock()) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) candidates.push_back(AI);
    }
    if (candidates.empty()) continue;

    std::vector<ReturnInst *> rets;
    if (freeOnReturn) {
      for (auto &I : instructions(F)) {
        if (auto *ri = dyn_cast<ReturnInst>(&I)) rets.push_back(ri);
      }
    }

    for (auto *AI : candidates) {
      // Over-aligned objects may exceed halloc's guarantee; skip them.
      if (AI->getAlign().value() > 16) continue;
      TypeSize ts = DL.getTypeAllocSize(AI->getAllocatedType());
      if (ts.isScalable()) continue;

      EscapeInfo esc = analyzeEscape(AI, hintsMode ? &hints : nullptr);
      if (esc.unsafe) continue;
      bool promote = hintsMode ? esc.toHinted : esc.taken;
      if (!promote) continue;

      // Drop lifetime/invariant markers: meaningless for a heap object.
      for (auto *marker : esc.lifetimeMarkers) marker->eraseFromParent();

      IRBuilder<> b(AI);
      Value *count = b.CreateZExtOrTrunc(AI->getArraySize(), i64Ty);
      Value *size =
          b.CreateMul(ConstantInt::get(i64Ty, ts.getFixedValue()), count, "promote.sz");
      CallInst *h = b.CreateCall(hallocFn, {size});
      h->takeName(AI);
      AI->replaceAllUsesWith(h);
      AI->eraseFromParent();
      promoted++;

      // Free at every return, preserving stack lifetime -- except when the
      // address itself is returned (that free would be a use-after-free).
      if (freeOnReturn && !esc.returned) {
        for (auto *ri : rets) {
          IRBuilder<> fb(ri);
          fb.CreateCall(hfreeFn, {h});
        }
      }
    }
  }

  if (promoted || hintsMode)
    alaska::println("[stack-promote] promoted ", promoted,
        " address-taken stack object(s) to the heap",
        freeOnReturn ? " (freed at return)" : " (lifetime via handle system)");

  return promoted ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
