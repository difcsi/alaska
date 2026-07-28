spec = False
import waterline as wl
import waterline.suites
import waterline.utils
import waterline.pipeline
import os
import time
import tempfile
from pathlib import Path
from waterline.run import Runner
import pandas as pd


# Runtime event metrics collected per run, in dump order (see EventCounters.hpp).
# The probe_* keys are only emitted by a cache-probe build (ALASKA_PROBE=1); they
# come back as 0 from plain/measurement builds.
EVENT_KEYS = ['halloc', 'hfree', 'incref', 'decref', 'gc_frees', 'compactions',
              'objects_moved', 'handles_total', 'handles_nonzero_rc',
              'probe_distinct_lines', 'probe_total_touches', 'probe_hits',
              'probe_misses', 'probe_oob', 'probe_sets',
              # Deferred reference counting (Levanoni-Petrank; ALASKA_DEFER_RC).
              'rc_deferred', 'rc_applied', 'rc_coalesced',
              'rc_overflow', 'rc_self_flushes', 'rc_log_hwm']


class EventCountingRunner(Runner):
    """Runner that also tallies runtime events: refcount inc/dec, GC frees, and
    heap-compaction passes / objects moved.

    Each run points $ALASKA_EVENT_LOG at a fresh temp file; a measurement build of
    libalaska (build with ALASKA_MEASURE=1, which sets ALASKA_ENABLE_EVENT_COUNTERS)
    writes its counts there at process exit. The parsed counts are merged into the
    per-run metric dict, so they flow through waterline into all.csv and their own
    <metric>.csv pivots alongside `time`. A plain timing build compiles the counters
    out and writes no file, so the counts simply come back as 0."""

    def run(self, workspace, config, binary):
        fd, path = tempfile.mkstemp(prefix='alaska-events-', suffix='.txt')
        os.close(fd)
        # Inject the log path for just this run, then restore (config is reused
        # across runs/pipelines).
        saved_env = config.env
        # Forward ALASKA_* runtime knobs (e.g. the cache-probe tuning vars
        # ALASKA_PROBE_SETS / ALASKA_PROBE_SPAN_GB) from the invoking shell into the
        # benchmark child. waterline's config.env does not carry the shell
        # environment, so without this a probe sweep would silently run every config
        # at the default set count.
        alaska_env = {k: v for k, v in os.environ.items() if k.startswith('ALASKA_')}
        config.env = {**(saved_env or {}), **alaska_env, 'ALASKA_EVENT_LOG': path}
        try:
            out = super().run(workspace, config, binary)
        finally:
            config.env = saved_env

        counts = {k: 0 for k in EVENT_KEYS}
        try:
            with open(path) as fh:
                for line in fh:
                    k, _, v = line.strip().partition('=')
                    if k in counts and v:
                        counts[k] = int(v)
        except FileNotFoundError:
            pass  # timing build: counters compiled out, no file written.
        finally:
            try:
                os.remove(path)
            except OSError:
                pass

        out.update(counts)
        return out

# Accumulates compile-time measurements recorded by TimedStage.
compile_times = []


class TimedStage(wl.pipeline.Stage):
    """Wraps any Stage to record wall-clock compilation time per benchmark."""

    def __init__(self, stage, pipeline_name, stage_name):
        self.stage = stage
        self.pipeline_name = pipeline_name
        self.stage_name = stage_name

    def run(self, input, output, benchmark):
        t0 = time.perf_counter()
        self.stage.run(input, output, benchmark)
        elapsed = time.perf_counter() - t0
        compile_times.append({
            'suite': benchmark.suite.name,
            'benchmark': benchmark.name,
            'pipeline': self.pipeline_name,
            'stage': self.stage_name,
            'compile_time': elapsed,
        })

import seaborn as sns
import matplotlib as mpl
from matplotlib.lines import Line2D
from matplotlib import cm
import matplotlib.pyplot as plt


try:
  from .utils import (activate_local_toolchain, find_spec, get_spec_size,
                      ALASKA_BUILD_CONFIGS, BASELINE_CONFIG, BASELINE_DRIVER,
                      config_prefix, vprint)
except ImportError:
  # Support running this file directly: `python benchmarks/figure7.py`.
  from utils import (activate_local_toolchain, find_spec, get_spec_size,
                    ALASKA_BUILD_CONFIGS, BASELINE_CONFIG, BASELINE_DRIVER,
                    config_prefix, vprint)

activate_local_toolchain()

# Construct the waterline workspace in the folder, `bench/`.
# This is where all benchmark sourcefiles and results will be saved.
space = wl.Workspace("bench")


# A linker bound to one Alaska install: its alaska-config emits that install's
# ldscript, runtime libraries (-lalaska/-lalaska_core) and an -rpath into its lib/,
# so each produced binary loads the right libalaska at run time with no per-config
# LD_LIBRARY_PATH needed.
class AlaskaLinker(wl.Linker):
  command = "clang++"

  def __init__(self, prefix):
    self.prefix = prefix
    self.linker_flags = os.popen(
        f"{prefix}/bin/alaska-config --ldflags").read().strip().split("\n")

  def link(self, ws, objects, output, args=[]):
    ws.shell("clang++", *args, '-ldl', *self.linker_flags, *objects, "-o", output)


# A compile stage bound to one Alaska install. With baseline=True it passes
# --baseline so alaska-transform skips every instrumentation pass (plain clang
# output); the install's feature set is then irrelevant, which is why baseline can
# borrow any install's driver.
class AlaskaStage(wl.pipeline.Stage):
  def __init__(self, prefix, extra_args=[], baseline=False):
    self.prefix = prefix
    self.extra_args = extra_args
    self.baseline = baseline

  def run(self, input, output, benchmark):
    vprint(f'alaska: compiling {output}', benchmark)
    aux_args = []
    if self.baseline:
      aux_args.append('--baseline')
    # A/B toggle for the induction-translate strength reduction (opt-in pass).
    # Set ALASKA_HOIST_INDUCTION=1 to enable it for the whole sweep without
    # editing code, so the same command reproduces both arms of the comparison.
    if not self.baseline and os.environ.get("ALASKA_HOIST_INDUCTION", "").lower() in ("1", "true", "yes", "on"):
      aux_args.append('--hoist-induction')
    env = os.environ.copy()
    if benchmark.suite.name == "SPEC2017":
      if benchmark.name == '602.gcc_s':
        # Handle GCC's funky garbage collector (Don't replace alloc_page)
        env['ALASKA_SPECIAL_CASE_GCC'] = 'true'
        vprint("Special case GCC allocator")

      if benchmark.name == '602.gcc_s' or benchmark.name == '600.perlbench_s':
        aux_args.append('--disable-hoisting')
        vprint("Disable hoisting!")

    space.shell(f"{self.prefix}/bin/alaska-transform",
                *aux_args, *self.extra_args, input, '-o', output, env=env)


class OptStage(waterline.pipeline.Stage):
  def __init__(self, passes=[]):
    self.passes = passes

  def run(self, input, output, benchmark):
    space.shell('opt', *self.passes, input, '-o', output)



# Sweep-size knobs. The full sweep (default) runs Embench at 10k iterations, GAP on a
# 2^19-node graph and NAS class B -- accurate but multi-hour across all six configs.
# Set ALASKA_BENCH_QUICK=1 for a representative subset that finishes in ~1h, or tune
# any knob individually (env overrides QUICK):
#   ALASKA_BENCH_QUICK   shrink everything for a ~1h sweep      (off by default)
#   ALASKA_SUITES        which suites to run, comma-separated   (embench,gap,nas,olden;
#                        `olden` (pointer-intensive) is now on by default and is
#                        quick-shrunk under ALASKA_BENCH_QUICK. Add `gcbench` for the
#                        alloc/free tree workloads that actually exercise hfree, or
#                        `mibench` for the embedded-workload suite)
#   ALASKA_BENCH         run only these benchmark(s) within the selected suites,
#                        comma-separated (e.g. binarytrees). Applies to suites that
#                        support per-benchmark selection (gcbench, olden); empty=all
#   ALASKA_EMBENCH_ITERS Embench iteration count                (full 10000, quick 1000)
#   ALASKA_GAP_SIZE      GAP graph is 2^SIZE nodes              (full 19,    quick 15)
#   ALASKA_NAS_CLASS     NAS problem size S<W<A<B<C             (full B,     quick W)
#   ALASKA_RUNS          timed repeats per benchmark            (default 2)
_quick = os.environ.get("ALASKA_BENCH_QUICK", "").lower() in ("1", "true", "yes", "on")
EMBENCH_ITERS = int(os.environ.get("ALASKA_EMBENCH_ITERS", "1000" if _quick else "10000"))
GAP_SIZE = os.environ.get("ALASKA_GAP_SIZE", "15" if _quick else "19")
NAS_CLASS = os.environ.get("ALASKA_NAS_CLASS", "W" if _quick else "B")
# In quick mode, skip the heavy NAS pseudo-applications (BT/SP/LU); they dominate
# the sweep's wall-clock even at class W. The lighter kernels still run.
NAS_EXCLUDE = ("bt", "sp", "lu") if _quick else ()
RUNS = int(os.environ.get("ALASKA_RUNS", "2"))
_suites = [s.strip() for s in os.environ.get("ALASKA_SUITES", "embench,gap,nas,olden").lower().split(",") if s.strip()]
# Per-benchmark selector: run only these named benchmarks within suites that
# support a `benchmarks=` subset (gcbench, olden). None => run the whole suite.
_bench_filter = [b.strip() for b in os.environ.get("ALASKA_BENCH", "").split(",") if b.strip()] or None
# Config (pipeline) subset: ALASKA_CONFIGS=refcount,refcount-gc restricts the sweep to those
# pipelines (include "baseline" to keep the plain-clang series). Empty => every config. A
# targeted A/B that touches only one service (e.g. the ALASKA_DEFER_RC knob, which affects only
# refcount* configs) can skip the rest of the matrix and finish far faster.
_configs_filter = [c.strip() for c in os.environ.get("ALASKA_CONFIGS", "").split(",") if c.strip()] or None
if _configs_filter is not None:
  _known = set(ALASKA_BUILD_CONFIGS) | {BASELINE_CONFIG}
  _unknown = [c for c in _configs_filter if c not in _known]
  if _unknown:
    print(f"warning: ALASKA_CONFIGS has unknown config(s) {_unknown}; known: {sorted(_known)}")

if "embench" in _suites:
  space.add_suite(wl.suites.Embench, iters=EMBENCH_ITERS)
if "gap" in _suites:
  space.add_suite(wl.suites.GAP, enable_openmp=False, enable_exceptions=False, graph_size=GAP_SIZE)
if "nas" in _suites:
  # Honor the per-benchmark selector (ALASKA_BENCH) for NAS too, so a single heavy
  # pseudo-app (e.g. sp) can be reproduced in isolation without editing this file.
  # NAS.configure only takes an exclude= list, so translate an inclusive filter into
  # "exclude everything not named", unioned with the quick-mode NAS_EXCLUDE.
  _nas_all = ("bt", "sp", "lu", "mg", "ft", "is", "cg", "ep")
  _nas_exclude = set(NAS_EXCLUDE)
  if _bench_filter is not None:
    _nas_exclude |= {b for b in _nas_all if b not in _bench_filter}
  space.add_suite(wl.suites.NAS, enable_openmp=False, suite_class=NAS_CLASS, exclude=tuple(_nas_exclude))
# Olden: pointer-intensive heap workloads that allocate AND free handle-linked
# structures -- the suite that actually exercises decref / dec-on-free / GC
# reclamation. On by default (in the default ALASKA_SUITES list). Quick mode shrinks
# every benchmark's problem size (see Olden.QUICK_ARGS), like the other suites.
if "olden" in _suites:
  space.add_suite(wl.suites.Olden, quick=_quick, benchmarks=_bench_filter)
# GCBench: self-contained tree-shaped alloc/free kernels (binarytrees, gcbench).
# Unlike Olden these genuinely call free() in steady state, so they are the suite
# that actually drives a nonzero program-initiated hfree (and, in refcount-gc
# configs, incref/decref + reclamation). Opt-in; quick mode drops a few tree levels.
if "gcbench" in _suites:
  space.add_suite(wl.suites.GCBench, quick=_quick, benchmarks=_bench_filter)
# MiBench: embedded/automotive/telecomm workloads. Opt-in (not in the default
# list). Uses the self-contained "large" dataset, or the smaller "small" dataset
# under quick mode.
if "mibench" in _suites:
  space.add_suite(wl.suites.MiBench, quick=_quick)

# Attempt to find SPEC2017 CPU on the system. Two sources, in priority order:
#   1. A distribution tarball (SPEC2017.tar.gz) in one of the find_spec() paths --
#      unpacked & installed fresh into the workspace.
#   2. An already-installed SPEC tree at the suite's DEFAULT_SPEC_DIR
#      (/usr/local/src/spec-cpu2017) -- copied into the workspace as-is.
# Previously only (1) was honored, so a machine with SPEC installed but no tarball
# silently skipped the whole suite.
_spec_install_dir = wl.suites.spec2017.DEFAULT_SPEC_DIR
spec = find_spec()
if spec:
  print('Found spec tarball here:', spec)
 # space.add_suite(wl.suites.SPEC2017,
  #                tar=spec,
   #               config=get_spec_size())
elif os.path.isdir(_spec_install_dir):
  print('Found installed spec here:', _spec_install_dir)
#  space.add_suite(wl.suites.SPEC2017,
    #              spec_dir=_spec_install_dir,
     #             config=get_spec_size())
else:
  print(f'SPEC2017 not found (no tarball and no install at '
        f'{_spec_install_dir}); skipping SPEC.')

space.clear_pipelines()


def add_pipeline(name, prefix, baseline=False):
  """Add one pipeline that compiles + links every benchmark against `prefix`.

  The pipeline name becomes the `config` column in the results, so each entry in
  the sweep below shows up as its own series in plotgen/figure7.py."""
  pl = waterline.pipeline.Pipeline(name)
  pl.add_stage(TimedStage(OptStage(['-O3']), name, 'Optimize'), name="Optimize")
  pl.add_stage(TimedStage(AlaskaStage(prefix, baseline=baseline), name, 'Alaska'),
               name="Alaska")
  pl.set_linker(AlaskaLinker(prefix))
  space.add_pipeline(pl)


# Which configs to actually run this sweep (ALASKA_CONFIGS subsets the matrix; None => all).
def _want_config(cfg):
  return _configs_filter is None or cfg in _configs_filter

# baseline: plain bundled clang via --baseline, borrowing one install's driver.
if _want_config(BASELINE_CONFIG):
  add_pipeline(BASELINE_CONFIG, config_prefix(BASELINE_DRIVER), baseline=True)
# One pipeline per Alaska build configuration (noservice, anchorage, refcount, ...).
for cfg in ALASKA_BUILD_CONFIGS:
  if _want_config(cfg):
    add_pipeline(cfg, config_prefix(cfg))


run_name = "figure7"
# EventCountingRunner adds the runtime event tallies (incref/decref/gc_frees/
# compactions/objects_moved) as extra metric columns in the results, in addition
# to the usual timing metrics. They are 0 unless libalaska was built with
# ALASKA_MEASURE=1 (the measurement build).
res = space.run(runs=RUNS, compile=True, run_name=run_name, runner=EventCountingRunner())

# Save compile-time measurements alongside the runtime results.
if compile_times:
    compile_df = pd.DataFrame(compile_times)
    compile_df.to_csv(f'bench/results/{run_name}/compile_times.csv', index=False)
    print(f"Compile times saved to bench/results/{run_name}/compile_times.csv")
