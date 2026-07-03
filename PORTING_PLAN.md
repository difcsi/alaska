# Porting `main-rc` features onto `dev` (→ `dev_rc`)

Status: **best-effort port applied (build-unverified — needs NOELLE + pinned LLVM).**

## Port status (what landed)

Committed on this branch, dependency-ordered. Everything new is gated so dev's
**default** build (cycle collection OFF) is behaviorally unchanged and the new,
unvalidated code compiles out of it.

| Area | State | Notes |
|---|---|---|
| Feature gates | ✅ | `ALASKA_ENABLE_{REFCOUNT,CYCLE_COLLECTION,EVENT_COUNTERS,CACHE_PROBE,DEFER_RC}` CMake switches → compile defs; cycle/defer→refcount checks. Cycle collection defaults **OFF** on dev. |
| Event counters | ✅ | `alaska/EventCounters.{hpp,cpp}` + gated hooks (inc/dec/halloc/hfree) + `atexit` dump + optional cache probe. |
| GC side bitmaps | ✅ | `alaska/gc_bitmaps.{hpp}` + `core/gc_bitmaps.cpp` (present + nullcount), indexed off `HandleTable::get_base()`. |
| Atomic refcount | ✅ | `Mapping::inc/dec_refcount` now whole-word CAS, **dev's Mapping layout preserved** (field mutated through the bitfield). |
| Zero-refcount reclaim | ✅ (gated) | `alaska_refcount_reclaim` + nullcount query API; uses dev's barrier handle-**pinning** instead of a separate present bitmap. |
| Cycle collector | ✅ (gated) | Bacon-Rajan trial deletion (`alaska/CycleCollector.{hpp,cpp}`), adapted to dev's ThreadCache/Runtime; C-API stubs when off. |
| Dec-on-free + copy-inc | ✅ (gated) | `alaska_hfree_dec_children` (drops an aggregate's child refs on free) + `alaska_inc_handles_in_range` (re-balances memcpy'd handle refs, emitted by RefcountInc). Uses dev's `ThreadCache::get_size`/`reverse_lookup` + `is_live_handle` + `could_be_aligned_handle`/`ALASKA_KEEP_HANDLE_ALIVE`. Defined always (links), work gated so dev's default free is unchanged and inc/dec stay balanced. Env opt-outs `ALASKA_NO_FREE_DEC` / `ALASKA_NO_COPY_INC`. |
| Compiler passes | ✅ | RefcountInc/Dec (advanced), EscapeAnalysis, StackPromote, HoistInductionTranslate, RefcountElision, keep-raw; wired into dev's pipeline/driver. |
| Tests / repro | ✅ | `test/refcount_*`, `keep_raw*`, `heap_hints*`, `repro/segdump.c` (standalone, dev has no CTest harness). |
| Build tools | ✅ | `tools/cmake/*`, `build_gclang.sh`, `get_llvm.sh` fixes. |

### Deliberately deferred / not ported (see inline PORT-NOTEs)

- **Mapping bit-layout redesign** — NOT done; dev keeps its own layout (its
  `pending_fault` bit and packed word are preserved). main-rc's nullcount HINT bit
  is dropped; the side bitmap is the source of truth.
- **Deferred (Levanoni–Petrank) increments** (`ALASKA_ENABLE_DEFER_RC`) — not ported
  (needs a per-thread deferred-inc log on ThreadCache). Gate exists but is inert.
  (Dec-on-free + copy-inc — previously listed here — are now ported; see the table.)
- **liballocs integration**, **Yukon runtime hooks**, **Perceus reuse cache** — not
  ported (the runtime `runtime/core/liballoc.c`, `liballocs_export.cpp`, huge-object
  refcount paths). Compiler-side StackPromote *is* in.
- **Benchmark/plot harness** (`plotgen/`, `benchmarks/`, `run_all.sh`) — main-only;
  dev has no such harness, so intentionally skipped.
- `a.json` crash dump — dropped (artifact).

### To validate / enable
1. Build with defaults → should match dev (new code gated out).
2. `cmake -DALASKA_ENABLE_CYCLE_COLLECTION=ON -DALASKA_ENABLE_EVENT_COUNTERS=ON`,
   then exercise `test/refcount_*`. Expect follow-up build fixes (unverified here).
3. Watch dev's disk-swap/Localizer/handle-fault behavior — the atomic-refcount CAS
   is the only change to a hot dev path in the default build.

---

## Original analysis

This document specifies how to rebuild `dev_rc` as `dev` + the feature stack that
`main-rc` adds on top of `main`. It exists because the port is **not** a
mechanical cherry-pick — `main` and `dev` are unrelated histories with divergent
runtime layouts, and the central feature (`Mapping`-embedded reference counting)
was implemented *differently* on each side. The reconciliation below has design
decisions that need a build + the test suite to validate, so it is written to be
executed against a real build environment rather than applied blind.

## 1. Branch topology (established facts)

| Branch    | Tip        | Notes |
|-----------|------------|-------|
| `main`    | `6eccf204` | CMake build, `runtime/core/*` + `runtime/include/alaska/*`, waterline submodule |
| `main-rc` | `8b9913cb` | `main` + 21 linear feature commits |
| `dev`     | `16a3fe59` | **Unrelated root** (`7434d6b7` vs main's `3b61488d`); `runtime/alaska/**` layout, Makefile + CMake |
| `dev_rc`  | `16a3fe59` | **Identical to `dev`** — the "stale" branch is just a copy with none of the RC features |

- `git merge-base main dev` is **empty** → no shared history; rebase/merge/cherry-pick
  all 3-way-merge against `main`'s tree, which mis-targets every `dev` file.
- `git diff main dev` = 402 files / ~93k lines. The two branches reorganized the
  runtime independently:
  - `main`/`main-rc`: `runtime/core/*.cpp` + `runtime/include/alaska/*.hpp`
  - `dev`: `runtime/alaska/**` (deep tree: `core/ heaps/ handles/ disk/ util/ compression/`)
- **dev already has its own refcount**: `compiler/passes/RefcountInc.cpp` /
  `RefcountDec.cpp`, `runtime/rt/refcount.cpp` (110 lines), plus `IntentPass.cpp`.
  main-rc's `refcount.cpp` is 490 lines and shares almost nothing with it.

## 2. The central blocker: `Mapping` bit-layout collision

Everything downstream (refcount, cycle collection, event counters, nullcount
reclaim) hangs off the `Mapping` word. The two designs conflict on the exact bits.

**dev** (`runtime/alaska/alaska.hpp`) — non-atomic bitfield, refcount embedded:
```
reserved : 2   // top pointer bits
refcount : 12
value    : 48
pinned   : 1
pending_fault : 1   // <-- dev's disk-swap / handle-fault subsystem uses this
```

**main-rc** (`runtime/include/alaska/alaska.hpp`) — packed union, all-atomic CAS:
```
value    : 47   // pointer in LOW bits (avoids overlap with flags)
refcount : 14
pinned   : 1    // bit 61
invl     : 1    // bit 62
swap     : 1    // bit 63  <-- REPURPOSED as kOnNullcountBit (zero-refcount hint)
```

Conflicts to resolve (design decision required, favoring main-rc's refcount per
the port's goal but **preserving dev's features**):
1. main-rc puts the pointer in the low 47 bits and repurposes bit 63; dev keeps a
   48-bit `value` + 2 `reserved` high bits. Picking main-rc's layout means
   re-checking every `get_pointer`/`set_pointer`/`encode`/`handle_id` site in dev.
2. dev's `pending_fault` bit has no home in main-rc's layout. It must be preserved
   (dev's disk swap + `alaska_ensure_present` depend on it) — likely by narrowing
   `refcount` to 13 bits or claiming one of main-rc's flag bits.
3. main-rc's mutators are atomic whole-word CAS; dev's are plain bitfield writes.
   Adopting atomics is required for main-rc's concurrent collector but interacts
   with dev's `invalidate()` (RISC-V `csrw`) ordering.

**This is the one item that must not be guessed.** It needs the maintainer's
intent on the swap/pending_fault bit and a build to confirm no site tears.

## 3. Feature commits (dependency order = commit order)

From `git log --oneline --reverse main..main-rc`:

```
b0c69467 feat: add refcount
7a9abcf4 feat: add null_map
0a8a785a feat: add null_map
cc8f8e04 feat: add Bacon-Rajan cycle collector and rework Mapping bit layout
d16ecc4c feat: add Yukon stack-to-heap promotion and gate refcount behind a config flag
2f8e7369 feat: integrate liballocs metadata queries and route cycle reclaim through stackscan
5edfb813 feat: extend liballocs integration to huge objects and handle compiler probes
aa5eeacf build: split refcount/cycle-collection/anchorage into independent feature gates
8d87d3b3 build: quiet benchmark harness output and fix libalaska translate export
17f85568 build: add refcount-anchorage config and quiet build.sh steps
8b478168 build: use raw --whole-archive for the libalaska translate link (CMake 3.13 compat)
b6774f10 bench: add env-tunable sweep-size knobs and a quick mode to figure7
2b0ba605 feat: add runtime event counters and heap-only refcount barriers
d4097472 perf: inline the refcount barriers and elide the first-store dec
277d2f75 feat: elide never-handle and self-copy refcount barriers
c6e90c88 feat: keep non-escaping malloc raw and harden conservative GC validation
9c35ac3a feat: add Perceus-style refcount<=1 reuse cache
7a8384ea feat: add deferred (Levanoni-Petrank) refcount increments and record-mode dec-on-free
5ccbad75 fix: reject misaligned mapping words in conservative scan; add segdump crash tool
5902bd51 feat: add opt-in hoist-induction-translate strength-reduction pass
8b9913cb chore: check in a.json crash-investigation dump   <-- DROP (crash-dump artifact)
```

Group them for integration:

- **F0 Mapping layout** (part of `cc8f8e04`) — see §2. Foundation for all refcount work.
- **F1 Refcount runtime** (`b0c69467`, `2b0ba605`, `d4097472`, `277d2f75`,
  `7a8384ea`): `runtime/rt/refcount.cpp`, event counters, inlined/elided barriers,
  deferred (Levanoni–Petrank) increments. Replaces dev's `runtime/rt/refcount.cpp`.
- **F2 Cycle collector** (`cc8f8e04`): `runtime/core/CycleCollector.{cpp,hpp}`,
  `gc_bitmaps.{cpp,hpp}`, Bacon–Rajan. Depends on F0.
- **F3 Event counters** (`2b0ba605`): `runtime/core/EventCounters.cpp` +
  `include/alaska/EventCounters.hpp`. Cross-cuts F1/F2 via gates.
- **F4 Yukon stack→heap promotion** (`d16ecc4c`): compiler `StackPromote.cpp` +
  `StackWriteBarrier.cpp`, runtime gating.
- **F5 liballocs integration** (`2f8e7369`, `5edfb813`): `runtime/rt/liballocs_export.cpp`,
  `runtime/core/liballoc.c`, huge-object path, compiler probe handling.
- **F6 Perceus reuse cache** (`9c35ac3a`): refcount≤1 reuse, `reusecache_test.cpp`.
- **F7 keep-raw non-escaping malloc + GC hardening** (`c6e90c88`): `EscapeAnalysis.{h,cpp}`,
  `keep_raw*` tests, conservative-scan validation.
- **F8 Hoist-induction-translate pass** (`5902bd51`): opt-in `HoistInductionTranslate.cpp`
  strength-reduction. Self-contained compiler pass.
- **F9 Misaligned-word scan fix + segdump tool** (`5ccbad75`): `repro/segdump.c`.
- **F10 Build/config gates** (`aa5eeacf`, `17f85568`, `8b478168`, `8d87d3b3`):
  independent feature gates `ALASKA_ENABLE_{REFCOUNT,CYCLE_COLLECTION,EVENT_COUNTERS,CACHE_PROBE}`.
  **Adapt to dev's CMake, not main's** (dev globs `alaska/**/*.cpp`, lists `rt/*.cpp`
  explicitly; dev also has a top-level Makefile).
- **F11 Bench/plot** (`b6774f10`, plus figure7/8 edits): `plotgen/figure7_compile.py`,
  `figure7_events.py`, `benchmarks/*`. Low risk, defer to last.
- **DROP**: `a.json` (1129-line crash dump), `null_map` may be subsumed by dev's
  handling — verify before porting the two `null_map` commits.

## 4. File path mapping (main-rc → dev)

New runtime sources drop into dev's globbed tree (auto-picked by CMake):

| main-rc path | dev target |
|---|---|
| `runtime/core/CycleCollector.cpp` + `include/alaska/CycleCollector.hpp` | `runtime/alaska/core/CycleCollector.cpp` (+ `.hpp` beside it) |
| `runtime/core/EventCounters.cpp` + `include/alaska/EventCounters.hpp` | `runtime/alaska/core/EventCounters.{cpp,hpp}` |
| `runtime/rt/gc_bitmaps.cpp` + `include/alaska/gc_bitmaps.hpp` | `runtime/rt/gc_bitmaps.cpp` (**add to explicit `SOURCES` list** in `runtime/CMakeLists.txt`) + header under `runtime/alaska/` |
| `runtime/rt/liballocs_export.cpp` | `runtime/rt/liballocs_export.cpp` (**add to `SOURCES`**) |
| `runtime/rt/refcount.cpp` | **replaces** `runtime/rt/refcount.cpp` (already in `SOURCES`) |
| `runtime/core/liballoc.c` | `runtime/alaska/core/liballoc.c` |
| `compiler/passes/*.cpp` (new) | same path (compiler trees are ~aligned) — **wire into dev's `compiler/CMakeLists.txt` + `passes/Alaska.cpp` pipeline** |
| `compiler/include/alaska/*.h` (new) | same path |
| `repro/segdump.c`, `test/*`, `plotgen/*` | same path |

Modified main-rc files whose dev counterpart differs structurally (hand-merge,
do **not** apply diffs blind): `runtime/*/ThreadCache.*`, `HandleTable.*`,
`Runtime.*`, `HugeObjectAllocator.*`, `SizedPage.*`, `Heap.*`, `translate.cpp`,
`rt/barrier.cpp`, `rt/init.cpp`, `rt/halloc.cpp`, `alaska.hpp`, `alaska.h`.

## 5. Build wiring (dev-specific)

- `runtime/CMakeLists.txt`: core is `file(GLOB_RECURSE ... alaska/**/*.cpp)` — new
  files under `runtime/alaska/` are auto-included. The `alaska` runtime lib uses an
  **explicit** `SOURCES` list (`rt/init.cpp rt/halloc.cpp rt/compat.c rt/barrier.cpp
  rt/refcount.cpp`) — append `rt/gc_bitmaps.cpp`, `rt/liballocs_export.cpp`.
- Feature gates: add `option(ALASKA_ENABLE_REFCOUNT ...)` etc. and translate main-rc's
  `configs/config.*.cmake` into dev's config mechanism.
- dev also ships a top-level **`Makefile`** — check whether it drives the build or
  just wraps CMake, and mirror any new options.
- `liballocs`/`libunwind`/`libzstd` link deps: dev already links `dl pthread zstd`
  (+ optional unwind). liballocs is a new external dep — gate it.

## 6. Verification that MUST happen (cannot be done in this environment)

This repo needs NOELLE + a pinned LLVM for the compiler and
liballocs/libzstd/libunwind for the runtime; a green build was not achievable here
(system LLVM is 18.1.3, no gclang/NOELLE). Required before merge:
1. Runtime builds with each feature gate **off** → byte-compatible with `dev`.
2. Runtime builds with gates **on**; `runtime/test/{cycle_test,reusecache_test}` pass.
3. Compiler builds; new passes load; `test/refcount_*`, `test/keep_raw*`,
   `test/sanity.c` pass.
4. dev's existing disk-swap / Localizer / handle-fault tests still pass (guards the
   §2 `pending_fault`/`swap` bit decision).

## 7. Recommended execution order

F10 (gates, no-op default) → F0 (Mapping, with maintainer sign-off on the bit
decision) → F3 (event counters) → F1 (refcount) → F2 (cycle) → F5 (liballocs) →
F7 (keep-raw/escape) → F6 (Perceus) → F4 (Yukon) → F8 (hoist) → F9 (segdump) →
F11 (bench/plot). Commit per group; build+test at F0, F1, F2 gates.
