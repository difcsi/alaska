# Generates the SPEC2017 data for figure 8: per-configuration overhead across the
# full benchmark sweep (baseline + every Alaska build config). Where figure 7 spans
# all suites, figure 8 zooms in on SPEC2017 so the per-config differences are legible.


import waterline as wl
import waterline.suites
import waterline.utils
import waterline.pipeline
import os
from pathlib import Path
from waterline.run import Runner
import pandas as pd

import seaborn as sns
import matplotlib as mpl
from matplotlib.lines import Line2D
from matplotlib import cm
import matplotlib.pyplot as plt

try:
  from .utils import (activate_local_toolchain, find_spec, get_spec_size,
                      ALASKA_BUILD_CONFIGS, BASELINE_CONFIG, BASELINE_DRIVER,
                      config_prefix)
except ImportError:
  from utils import (activate_local_toolchain, find_spec, get_spec_size,
                    ALASKA_BUILD_CONFIGS, BASELINE_CONFIG, BASELINE_DRIVER,
                    config_prefix)

activate_local_toolchain()

# Construct the waterline workspace in the folder, `bench/`.
# This is where all benchmark sourcefiles and results will be saved.
space = wl.Workspace("bench")


# See benchmarks/figure7.py for the rationale behind binding each linker/stage to a
# specific install prefix.
class AlaskaLinker(wl.Linker):
  command = "clang++"

  def __init__(self, prefix):
    self.prefix = prefix
    self.linker_flags = os.popen(
        f"{prefix}/bin/alaska-config --ldflags").read().strip().split("\n")

  def link(self, ws, objects, output, args=[]):
    ws.shell("clang++", *args, '-ldl', *self.linker_flags, *objects, "-o", output)


class AlaskaStage(wl.pipeline.Stage):
  def __init__(self, prefix, extra_args=[], baseline=False):
    self.prefix = prefix
    self.extra_args = extra_args
    self.baseline = baseline

  def run(self, input, output, benchmark):
    aux_args = []
    if self.baseline:
      aux_args.append('--baseline')
    env = os.environ.copy()
    if benchmark.suite.name == "SPEC2017":
      if benchmark.name == '602.gcc_s':
        # Handle GCC's funky garbage collector (Don't replace alloc_page)
        env['ALASKA_SPECIAL_CASE_GCC'] = 'true'
        print("Special case GCC allocator")

      if benchmark.name == '602.gcc_s' or benchmark.name == '600.perlbench_s':
        aux_args.append('--disable-hoisting')
        print("Disable hoisting!")

    space.shell(f"{self.prefix}/bin/alaska-transform",
                *aux_args, *self.extra_args, input, '-o', output, env=env)



class OptStage(waterline.pipeline.Stage):
  def __init__(self, passes=[]):
    self.passes = passes

  def run(self, input, output, benchmark):
    space.shell('opt', *self.passes, input, '-o', output)


spec = find_spec()
if spec:
  print('Found spec here:', spec)
  space.add_suite(wl.suites.SPEC2017,
                  tar=spec,
                  # Disable perlbench and gcc
                  disabled=[600, 602],
                  config=get_spec_size())

space.clear_pipelines()


def add_pipeline(name, prefix, baseline=False):
  pl = waterline.pipeline.Pipeline(name)
  pl.add_stage(OptStage(['-O3']), name="Optimize")
  pl.add_stage(AlaskaStage(prefix, baseline=baseline), name="Alaska")
  pl.set_linker(AlaskaLinker(prefix))
  space.add_pipeline(pl)


# baseline + one pipeline per Alaska build configuration, same as figure 7.
add_pipeline(BASELINE_CONFIG, config_prefix(BASELINE_DRIVER), baseline=True)
for cfg in ALASKA_BUILD_CONFIGS:
  add_pipeline(cfg, config_prefix(cfg))

res = space.run(runs=2, compile=True, run_name="figure8")
