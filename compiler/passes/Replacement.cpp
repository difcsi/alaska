#include <alaska/EscapeAnalysis.h>
#include <alaska/Passes.h>
#include <alaska/Translations.h>
#include <alaska/Utils.h>
#include <alaska/WrappedFunctions.h>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

// Some functions should be blacklisted from having allocation
static bool is_allocation_blacklisted(const llvm::StringRef &name) {
  // XALAN
  if (name == "_ZN11xalanc_1_1025XalanMemoryManagerDefault8allocateEm") return true;


  // LEELA
  if (name == "_ZN3GTP7executeER9GameStateNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
    return true;
  if (name ==
      "_ZNSt15__new_allocatorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEE8allocateEmPKv")
    return true;
  if (name ==
      "_ZNSt15__new_allocatorIN5boost6tuples5tupleIiiP7UCTNodeNS1_9null_typeES5_S5_S5_S5_S5_S5_"
      "EEE8allocateEmPKv")
    return true;


  if (getenv("ALASKA_SPECIAL_CASE_GCC")) {
    alaska::println("SPECIAL CASE? ", name);
    if (name == "alloc_page") {
      alaska::println("XXX SPECIAL CASE GCC page_alloc XXX\n");
      return true;
    }
  }
  return false;
}

// Take any calls to a function called `original_name` and
// replace them with a call to `new_name` instead.
//
// `dontReplace` (optional): a set of call instructions to leave on the original
// function. Used by the keep-raw optimization to keep a non-escaping allocation
// -- and the free/usable_size calls that consume it -- on the libc allocator.
static void replace_function(Module &M, std::string original_name, std::string new_name = "",
    bool is_allocator = false, const SmallPtrSetImpl<Instruction *> *dontReplace = nullptr) {
  if (new_name == "") {
    new_name = "alaska_wrapped_" + original_name;
  }
  auto oldFunction = M.getFunction(original_name);


  if (oldFunction) {
    auto newFunction = M.getOrInsertFunction(new_name, oldFunction->getFunctionType()).getCallee();
    // oldFunction->replaceAllUsesWith(newFunction);
    oldFunction->replaceUsesWithIf(newFunction, [&](llvm::Use &use) -> bool {
      // Leave call sites the keep-raw pass selected on the original allocator.
      if (dontReplace) {
        if (auto inst = dyn_cast<Instruction>(use.getUser())) {
          if (dontReplace->count(inst)) return false;
        }
      }
      if (is_allocator) {
        auto user = use.getUser();
        if (auto inst = dyn_cast<Instruction>(user)) {
          auto callingFunc = inst->getFunction();
          if (is_allocation_blacklisted(callingFunc->getName())) {
            alaska::println("Not replacing ", original_name, " in ", callingFunc->getName(),
                " because it is blackisted");
            return false;
          }
        }
      }
      return true;
    });
  }
}


// True if `v` derives from one of `producers` through only address-preserving
// casts/geps (no phi/select merge, no other origin). Used to confirm a
// free/realloc/usable_size call consumes exactly a kept-raw region pointer and
// nothing that could be a handle.
static bool traces_to_producer(Value *v, const SmallPtrSetImpl<Value *> &producers) {
  while (!producers.count(v)) {
    if (auto *bc = dyn_cast<BitCastInst>(v)) {
      v = bc->getOperand(0);
      continue;
    }
    if (auto *asc = dyn_cast<AddrSpaceCastInst>(v)) {
      v = asc->getOperand(0);
      continue;
    }
    if (auto *gep = dyn_cast<GetElementPtrInst>(v)) {
      v = gep->getPointerOperand();
      continue;
    }
    return false;
  }
  return true;
}


// Find malloc/calloc allocations whose region (the allocation, any realloc that
// grows it, and the free/usable_size calls that consume it) provably does not
// escape its function, and record that whole region in `dontReplace` so the
// replacement leaves it on the libc allocator. Every producing call (the malloc
// and each realloc result) is tagged with `!alaska.keepraw` so the translation
// analyses treat it like an `alloca` (see shouldTranslate / PointerFlowGraph).
// On by default; a no-op when ALASKA_NO_KEEP_RAW is set.
static void computeKeepRaw(Module &M, SmallPtrSetImpl<Instruction *> &dontReplace) {
  // Keep-raw runs by default. ALASKA_NO_KEEP_RAW disables it at compile-driver
  // time (the pass reads it via getenv; opt inherits the env), so a sweep can A/B
  // baseline vs keep-raw from a single toolchain without rebuilding the compiler.
  if (getenv("ALASKA_NO_KEEP_RAW") != NULL) return;

  // Allocation roots eligible to stay raw. realloc is not a root (it is reached
  // as part of a chain rooted at a malloc/calloc). The embench *_beebs aliases
  // are excluded: their free_beebs is not in the allocator whitelist, so they
  // would always disqualify anyway.
  static const std::set<StringRef> raw_candidates = {"malloc", "calloc"};

  MDNode *keepRawMD = MDNode::get(M.getContext(), {});
  unsigned long kept = 0, chains = 0;

  for (auto &F : M) {
    if (F.empty()) continue;
    for (auto &I : llvm::instructions(F)) {
      auto *call = dyn_cast<CallInst>(&I);
      if (!call) continue;
      auto *callee = call->getCalledFunction();
      if (!callee || raw_candidates.find(callee->getName()) == raw_candidates.end()) continue;

      alaska::EscapeInfo esc = alaska::analyzeEscape(call, /*hints=*/nullptr,
          /*allocatorCallsAreUses=*/true, /*followReallocChain=*/true, /*checkNoCapture=*/true);

      // Keep raw only when the region never escapes, is never returned, and is
      // not used in an ABI-sensitive way.
      if (esc.taken || esc.returned || esc.unsafe) continue;

      // Every realloc/free/usable_size call in the region must consume a pointer
      // that traces (through casts/geps only -- no phi/select merge) back to a
      // region producer, so a raw allocator call can never receive a non-region
      // (possibly handle) pointer.
      bool clean = true;
      for (auto *c : esc.reallocUses)
        if (!traces_to_producer(c->getArgOperand(0), esc.regionProducers)) { clean = false; break; }
      if (clean)
        for (auto *c : esc.deallocUses)
          if (!traces_to_producer(c->getArgOperand(0), esc.regionProducers)) { clean = false; break; }
      if (!clean) continue;

      // Commit: keep the whole region (allocation + reallocs + frees) on libc,
      // and tag every producer so the translate analyses treat it like an alloca.
      for (auto *p : esc.regionProducers)
        if (auto *pc = dyn_cast<CallInst>(p)) pc->setMetadata("alaska.keepraw", keepRawMD);
      dontReplace.insert(call);
      for (auto *c : esc.reallocUses) dontReplace.insert(c);  // CallBase* -> Instruction*
      for (auto *c : esc.deallocUses) dontReplace.insert(c);
      kept++;
      if (!esc.reallocUses.empty()) chains++;
    }
  }

  if (kept)
    alaska::println("[keep-raw] kept ", kept, " non-escaping allocation(s) raw (", chains,
        " realloc chain(s))");
}


PreservedAnalyses AlaskaReplacementPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Decide, before any replacement, which non-escaping allocations (and the
  // free/usable_size calls that consume them) to leave on the libc allocator.
  // Must run while the calls are still named malloc/calloc. On by default;
  // ALASKA_NO_KEEP_RAW makes computeKeepRaw a no-op (dontReplace stays empty, so
  // no call site is skipped).
  SmallPtrSet<Instruction *, 32> dontReplace;
  computeKeepRaw(M, dontReplace);

  if (getenv("ALASKA_NO_REPLACE_MALLOC") == NULL) {
    if (getenv("ALASKA_SPECIAL_CASE_GCC") != NULL) {
      // I hate GCC. I hate this (FIXME)
      replace_function(M, "xmalloc", "malloc", false);
      replace_function(M, "xcalloc", "calloc", false);
      replace_function(M, "xrealloc", "realloc", false);


      alaska::println("XXX SPECIAL CASE GCC XXX\n");
      replace_function(M, "xmalloc", "halloc", true);
      replace_function(M, "xcalloc", "hcalloc", true);
      replace_function(M, "xrealloc", "hrealloc", true);
    }

    replace_function(M, "malloc", "halloc", true, &dontReplace);
    replace_function(M, "calloc", "hcalloc", true, &dontReplace);
    replace_function(M, "realloc", "hrealloc", true, &dontReplace);

    replace_function(M, "malloc_beebs", "halloc", true);     // embench
    replace_function(M, "calloc_beebs", "hcalloc", true);    // embench
    replace_function(M, "realloc_beebs", "hrealloc", true);  // embench

    // NOTE: C++ operator new (_Znwm/_Znam) is intentionally NOT replaced. Routing
    // it to halloc makes its result a handle, but the translation layer never
    // translates those uses -- shouldTranslate() explicitly excludes _Znam
    // (Translations.cpp) and the allocator analyses only know malloc/calloc/realloc
    // -- so the program dereferences a raw handle and segfaults. Supporting C++
    // allocation needs the translation analysis to recognize operator new AND
    // new[]-array-cookie handling; until then operator new stays on the system
    // allocator (C++ benchmarks simply aren't handle-managed).
  }

  // even if calls to malloc are not replaced, we still ought to replace these functions for
  // compatability. Calling hfree() with a non-handle will fall back to the system's free() - same
  // for alaska_usable_size().
  replace_function(M, "free", "hfree", false, &dontReplace);
  replace_function(M, "free_beebs", "hfree");  // embench
  replace_function(M, "malloc_usable_size", "alaska_usable_size", false, &dontReplace);

  // operator delete (_ZdlPv/_ZdaPv) is intentionally NOT replaced -- it is the
  // pair of the operator new replacement above, which is disabled (see there).
  // Routing delete to hfree while new stays on the system allocator is harmless
  // (hfree falls back to free for non-handles) but pointless, so leave it.

  for (auto *name : alaska::wrapped_functions) {
    replace_function(M, name);
  }

  for (auto &F : M) {
    if (llvm::verifyFunction(F, &errs())) {
      errs() << "Function verification failed!\n";
      errs() << F.getName() << "\n";
      auto l = alaska::extractTranslations(F);
      if (l.size() > 0) {
        alaska::printTranslationDot(F, l);
      }
      exit(EXIT_FAILURE);
    }
  }
  return PreservedAnalyses::none();
}
