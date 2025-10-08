# Alaska LLVM Passes

The Alaska compiler plugin extends LLVM with a collection of custom passes that prepare
programs for execution under the handle-based Alaska runtime. This document describes
each pass that lives under `compiler/passes`, how the pass manager pipeline is wired
up in `compiler/passes/Alaska.cpp`, and the toggles that influence their behaviour.

All of the passes are registered with LLVM's new pass manager. They can be invoked by
adding `-load-pass-plugin <path-to-libAlaska.so>` to your `opt`, `clang`, or `llc`
command line and composing the pipelines listed below.

## Pipeline Entry Points

The plugin registers the following pipeline names (see `compiler/passes/Alaska.cpp`):

- `alaska-prepare` – `mem2reg`-style clean-up (`DCE`, `ADCE`), `WholeProgramDevirt`,
  `SimpleFunctionPass`, then `AlaskaNormalizePass`.
- `alaska-replace` – runs `AlaskaReplacementPass` to redirect allocator and wrapped calls.
- `alaska-translate` – runs `AlaskaTranslatePass` with hoisting enabled and follows with
  `AlaskaIntentPass` (debug hook).
- `alaska-translate-nohoist` – same as above but disables hoisting inside
  `AlaskaTranslatePass`; useful when strict-aliasing assumptions are unsafe.
- `alaska-escape` – runs `AlaskaEscapePass`, forcing handles to be translated before
  escaping to external code.
- `alaska-lower` – lowers Alaska intrinsics with `AlaskaLowerPass`.
- `alaska-inline` – inlines helper functions through `TranslationInlinePass`.
- `alaska-tracking` – (optionally `TranslationPrinterPass`), `PlaceSafepointsPass`,
  `HandleFaultPass`, and `PinTrackingPass`.
- `alaska-type-infer` – placeholder for `OptimisticTypesPass` (currently not scheduled).

Typical lowering uses these passes in order:

```sh
opt -load-pass-plugin <libAlaska.so> \
    -passes=alaska-prepare,alaska-replace,alaska-translate,alaska-escape,alaska-lower,alaska-inline,alaska-tracking \
    < input.ll > output.ll
```

## Pass Reference

### AlaskaNormalizePass (`compiler/passes/Normalize.cpp`)

Normalises pointer-manipulating IR to simplify later analysis:

- Reifies `bitcast` and `getelementptr` operators used by loads/stores into explicit
  instructions so that every pointer-producing value has instruction provenance.
- Expands pointer-typed `select` instructions into control-flow splits with PHI nodes,
  which makes rooting and dominance reasoning easier.

The pass runs as part of `alaska-prepare` and does not introduce runtime calls. It
returns `PreservedAnalyses::none()` to signal that downstream passes should not assume
existing analyses remain valid.

### SimpleFunctionPass (`compiler/passes/Alaska.cpp`)

Marks functions without loops and with only intrinsic or trivial calls as
`alaska_is_simple`. `PinTrackingPass` trusts this attribute to skip inserting GC
statepoints for simple leaf functions, which removes unnecessary safepoint overhead.
The pass is scheduled inside `alaska-prepare`.

### AlaskaReplacementPass (`compiler/passes/Replacement.cpp`)

Redirects allocation and selected library calls to Alaska-aware wrappers:

- Replaces `malloc`/`calloc`/`realloc` (and several project-specific variants) with
  handle-aware `halloc`/`hcalloc`/`hrealloc`.
- Optional GCC-specific remapping is guarded by `ALASKA_SPECIAL_CASE_GCC`.
- Respects `ALASKA_NO_REPLACE_MALLOC` to leave the standard allocators untouched.
- Always rewrites `free` (and friends) to `hfree` so that freeing non-handles falls back
  gracefully.
- Handles an extensible list of wrapped functions in `alaska::wrapped_functions`.

The pass verifies every transformed function and aborts on verification failures, making
it a good early-stage sanity check.

### AlaskaTranslatePass (`compiler/passes/TranslatePass.cpp`)

The core pass that inserts calls to the runtime translation API:

- Skips functions that already contain the `noelle` metadata key `alaska` to avoid
  double-processing.
- Hoists translation calls (`alaska.translate`/`alaska.release`) towards allocation sites
  when safe; falls back to block-local insertion when hoisting is disabled.
- Honors the `ALASKA_NO_STRICT_ALIAS` environment variable by automatically disabling
  hoisting when strict-aliasing assumptions may be invalid.
- Ignores runtime functions marked with the `$__ALASKA__` section tag.

Translation metadata drives later passes such as `PinTrackingPass`. The pass is part of
both `alaska-translate` pipelines and uses helper routines defined in
`compiler/lib/Translations.cpp`.

### AlaskaIntentPass (`compiler/passes/IntentPass.cpp`)

Debug-oriented pass that currently collects translation traces for the function
named `search` and prints DOT graphs when `ALASKA_DUMP_TRANSLATIONS` is enabled. The
logic is intentionally conservative and exits early; treat it as a hook for developers
who are investigating translation behaviour rather than production infrastructure.

### AlaskaEscapePass (`compiler/passes/Escape.cpp`)

Ensures that any handle potentially escaping to blocking or unknown code is translated
and locked before the call:

- Builds a map from external functions to the set of arguments that must be escaped,
  marking optionally varargs when `va_start` is used. `ALASKA_RELAX_VARARG` disables
  conservative vararg escaping.
- Tracks a curated allowlist of library calls that are known not to block; everything
  else is conservatively treated as potentially blocking.
- Annotates declarations with `alaska_escape` and `alaska_mightblock` attributes,
  which lower passes use to pick runtime stubs or to record safepoint metadata.
- Inserts root/translate/release sequences around arguments that should be converted to
  handles before crossing the call boundary.

Schedule this pass after `alaska-translate` so that translation metadata is available.

### AlaskaLowerPass (`compiler/passes/Lower.cpp`)

Lowers high-level intrinsics produced by earlier passes to the runtime ABI:

- Replaces `alaska.safepoint_poll` with `alaska_safepoint`.
- Eliminates `alaska.root` and `alaska.release` markers once their effects have been
  materialised.
- Switches the `alaska.translate` intrinsic to either `alaska_translate` or
  `alaska_translate_escape`, depending on usage patterns.
- Replaces `alaska.derive` nodes with explicit `getelementptr` instructions to preserve
  pointer arithmetic in the final IR.

The pass cleans up auxiliary landing pads and optionally runs `verifyFunction` (guarded
by `ALASKA_VERIFY_PASS`). It is the canonical lowering step before code generation.

### TranslationInlinePass (`compiler/passes/Alaska.cpp`)

Inlines calls to helper functions whose names start with `alaska_`. This pass reduces
indirection for tiny runtime shims that the translator emits and typically follows
`AlaskaLowerPass` via `alaska-inline`.

### PlaceSafepointsPass (`compiler/passes/PlaceSafepoints.cpp`)

A fork of LLVM's upstream safepoint placer with Alaska-specific defaults:

- Inserts safepoint polls on entry and loop backedges unless disabled by flags such as
  `alaska-spp-no-entry`, `alaska-spp-no-backedge`, or `alaska-spp-no-call`.
- Uses command-line flags (e.g. `alaska-spp-all-backedges`, `alaska-spp-counted-loop-trip-width`)
  to control heuristics, mirroring LLVM's original pass.

The pass is scheduled within `alaska-tracking` just before the runtime-specific
statepoint rewrites.

### PinTrackingPass (`compiler/passes/PinTracking.cpp`)

Augments functions with precise GC statepoint metadata and stack slot tracking:

- Skips functions tagged `alaska_is_simple` by `SimpleFunctionPass`.
- Forces the function to use the `coreclr` GC strategy and emits the `.text.alaska` section.
- Collects translations whose lifetimes intersect calls or invokes that might trigger a
  safepoint, assigns each a slot in a stack-allocated array, and stores the handle prior
  to the call.
- Wraps potentially blocking `call` and `invoke` instructions in
  `gc.statepoint` intrinsics (`CreateGCStatepointCall/Invoke`), converting return values
  through `gc.result`.
- Tags special calls (e.g. barrier polls, handle-fault checks) with specific IDs and patch
  sizes expected by the runtime.

Verification is enforced after rewriting. This pass must run after both translation and
escape handling have annotated the IR.

### HandleFaultPass (`compiler/passes/PinTracking.cpp`)

Transforms calls to `alaska_do_handle_fault_check` into synthetic safepoints that invoke
an injected `alaska.fault` stub. This ensures that fault checks participate in stack map
generation and cooperate with the runtime's patching strategy. The pass runs immediately
after `PlaceSafepointsPass` within the `alaska-tracking` pipeline.

### TranslationPrinterPass (`compiler/passes/TranslationPrinter.cpp`)

When `ALASKA_DUMP_TRANSLATIONS` is defined (and optionally filtered via
`ALASKA_DUMP_TRANSLATIONS_FOCUS`), prints the translation forest of each function to
`stderr` in DOT format. The pass is conditionally scheduled at the front of
`alaska-tracking` and is intended for debugging.

### AlaskaArgumentTracePass (`compiler/passes/ArgumentTrace.cpp`)

Instruments function entries with calls to `alaska_argtrack` to log pointer arguments.
It materialises the function name as a private global string and passes both the original
argument values and sentinels for non-pointer parameters. The pass currently is not wired
into a named pipeline; run it explicitly if argument tracing is required.

### RedundantArgumentPinElisionPass (`compiler/passes/ArgPinElision.cpp`)

Prototype machinery for cloning functions and adding extra opaque pointer parameters so
that redundant handle pins can be elided. The logic is inactive (the pass exits early and
preserves all analyses) but the scaffolding remains for future experimentation.

## Related Helpers

- `ProgressPass` (in `Alaska.cpp`) is a lightweight timing utility that prints progress
  messages when `print_progress` is true. It is not currently wired into the default
  pipelines.
- `OptimisticTypesPass` (in `Alaska.cpp`) drives the optimistic type inference engine in
  `compiler/lib/OptimisticTypes.cpp`. The infrastructure exists but the pass is not added
  to any exposed pipeline.

Keep these helpers in mind when prototyping new pipeline stages—they can be registered
with PassBuilder in the same fashion as the existing passes.
