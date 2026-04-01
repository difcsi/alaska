import waterline as wl
import waterline.suites
import waterline.utils
import waterline.pipeline
import os
import time
from pathlib import Path
from waterline.run import Runner
import pandas as pd

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
  from .utils import activate_local_toolchain, find_spec, get_spec_size
except ImportError:
  # Support running this file directly: `python benchmarks/figure7.py`.
  from utils import activate_local_toolchain, find_spec, get_spec_size

activate_local_toolchain()

# Construct the waterline workspace in the folder, `bench/`.
# This is where all benchmark sourcefiles and results will be saved.
space = wl.Workspace("bench")

# Grab alaska's linker flags using it's config tool.
linker_flags = os.popen("alaska-config --ldflags").read().strip().split("\n")

class AlaskaLinker(wl.Linker):
  command = "clang++"
  def link(self, ws, objects, output, args=[]):
    ws.shell("clang++", *args, '-ldl', *linker_flags, *objects, "-o", output)


class AlaskaStage(wl.pipeline.Stage):
  def __init__(self, extra_args = []):
    self.extra_args = extra_args

  def run(self, input, output, benchmark):
    print(f'alaska: compiling {output}', benchmark)
    aux_args = []
    env = os.environ.copy()
    if benchmark.suite.name == "SPEC2017":
      if benchmark.name == '602.gcc_s':
        # Handle GCC's funky garbage collector (Don't replace alloc_page)
        env['ALASKA_SPECIAL_CASE_GCC'] = 'true'
        print("Special case GCC allocator")

      if benchmark.name == '602.gcc_s' or benchmark.name == '600.perlbench_s':
        aux_args.append('--disable-hoisting')
        print("Disable hoisting!")
    
    space.shell(f"alaska-transform", *aux_args, *self.extra_args, input, '-o', output, env=env)


class AlaskaBaselineStage(wl.pipeline.Stage):
  def run(self, input, output, benchmark):
    space.shell(f"alaska-transform", "--baseline", input, '-o', output)


class OptStage(waterline.pipeline.Stage):
  def __init__(self, passes=[]):
    self.passes = passes

  def run(self, input, output, benchmark):
    space.shell('opt', *self.passes, input, '-o', output)



# Add the Embench suite to the workspace.
space.add_suite(wl.suites.Embench)
# Add the GAPBS suite to the workspace.
space.add_suite(wl.suites.GAP, enable_openmp=False, enable_exceptions=False, graph_size='19')
# Add the NAS suite to the workspace.
space.add_suite(wl.suites.NAS, enable_openmp=False, suite_class="B")

# Attempt to find SPEC2017 CPU on the system.
spec = find_spec()
if spec:
  # If we found spec, add it to the suite
  print('Found spec here:', spec)
  space.add_suite(wl.suites.SPEC2017,
                  tar=spec,
                  config=get_spec_size())

space.clear_pipelines()



pl = waterline.pipeline.Pipeline("alaska")
pl.add_stage(TimedStage(OptStage(['-O3']), 'alaska', 'Optimize'), name="Optimize")
pl.add_stage(TimedStage(AlaskaStage(), 'alaska', 'Alaska'), name="Alaska")
pl.set_linker(AlaskaLinker())
space.add_pipeline(pl)


pl = waterline.pipeline.Pipeline("baseline")
pl.add_stage(TimedStage(OptStage(['-O3']), 'baseline', 'Optimize'), name="Optimize")
pl.add_stage(TimedStage(AlaskaBaselineStage(), 'baseline', 'Baseline'), name="Baseline")
pl.set_linker(AlaskaLinker())
space.add_pipeline(pl)


run_name = "figure7"
res = space.run(runs=2, compile=True, run_name=run_name)

# Save compile-time measurements alongside the runtime results.
if compile_times:
    compile_df = pd.DataFrame(compile_times)
    compile_df.to_csv(f'bench/results/{run_name}/compile_times.csv', index=False)
    print(f"Compile times saved to bench/results/{run_name}/compile_times.csv")
