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

#include <alaska/EscapeAnalysis.h>
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
  // PORT-NOTE: dev's LLVM dropped StringRef::startswith; use starts_with (the
  // spelling used everywhere else in dev's compiler tree).
  if (n.starts_with("alaska") || n.starts_with("__alaska") ||
      n.starts_with("anchorage") || n.starts_with("yukon") || n.starts_with("llvm."))
    return true;
  if (F.getSection().starts_with("$__ALASKA__")) return true;
  if (n == "halloc" || n == "hcalloc" || n == "hrealloc" || n == "hfree" ||
      n == "malloc" || n == "calloc" || n == "realloc" || n == "free" ||
      n == "alaska_usable_size")
    return true;
  return false;
}

// `EscapeInfo` and `analyzeEscape` live in <alaska/EscapeAnalysis.h>, shared with
// AlaskaReplacementPass. StackPromote uses the default (allocator-unaware) mode,
// so its behavior is unchanged by the extraction.

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

      alaska::EscapeInfo esc = alaska::analyzeEscape(AI, hintsMode ? &hints : nullptr);
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
