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


# from .runner import PerfRunner, SizeRunner

import shutil
import os

space = wl.Workspace("bench")

enable_openmp = False

linker_flags = os.popen("alaska-config --ldflags").read().strip().split("\n")


class AlaskaLinker(wl.Linker):
  command = "clang++"
  def link(self, ws, objects, output, args=[]):
    ws.shell("clang++", *args, '-ldl', *linker_flags, *objects, "-o", output)


class AlaskaStage(wl.pipeline.Stage):
  def __init__(self, extra_args = []):
    self.extra_args = extra_args

  def run(self, input, output, _):
    print(f'alaska: compiling {output}')
    space.shell(f"alaska-transform", *self.extra_args, input, '-o', output)


class AlaskaGCCSpecialCaseStage(wl.pipeline.Stage):
  def __init__(self, extra_args = []):
    self.extra_args = extra_args

  def run(self, input, output, _):
    env = os.environ.copy()
    env['ALASKA_SPECIAL_CASE_GCC'] = 'true'
    space.shell(f"alaska-transform", *self.extra_args, input, '-o', output, env=env)

class AlaskaNoStrictAliasStage(wl.pipeline.Stage):
  def run(self, input, output, benchmark):
    env = os.environ.copy()
    env['ALASKA_NO_STRICT_ALIAS'] = 'true'
    print(f'alaska: compiling {input} -> {output}')
    space.shell(f"alaska-transform", input, '-o', output, env=env)

class AlaskaBaselineStage(wl.pipeline.Stage):
  def run(self, input, output, benchmark):
    print(f'alaska: baseline compiling {output}')
    space.shell(f"alaska-transform", "--baseline", input, '-o', output)


class OptStage(waterline.pipeline.Stage):
  def __init__(self, passes=[]):
    self.passes = passes

  def run(self, input, output, benchmark):
    print('opt ', input)
    space.shell('opt', *self.passes, input, '-o', output)


all_spec = [
    600,
    602,
    605,
    620,
    623,
    625,
    631,
    641,
    657,
    619,
    638,
    644,
]\1

spec_enable = [
  605, # mcf
  # 623, # xalancbmk
  # 625, # x264
  # 631,
  # 641,
  # 657,
  # 619,
  # 638, # imagick
  # 644, # nab
  
  # 600, # perlbench
  # 602, # gcc
]





spec_locations = [
    "./SPEC2017.tar.gz",
    "~/SPEC2017.tar.gz",
    "/SPEC2017.tar.gz",
]
def find_spec():
    print('looking for SPEC in these locations:', spec_locations)
    for loc in spec_locations:
        loc = os.path.expanduser(loc)
        if os.path.isfile(loc):
            return loc
    return None


# space.add_suite(wl.suites.Embench)
# space.add_suite(wl.suites.GAP, enable_openmp=False, enable_exceptions=False, graph_size='19')
# space.add_suite(wl.suites.NAS, enable_openmp=False, suite_class="B")

do_spec = True
spec = find_spec()
if spec and do_spec:
    print('found spec:', spec)
    space.add_suite(wl.suites.SPEC2017,
                    tar=spec,
                    disabled=[t for t in all_spec if t not in spec_enable],
                    config="test")
else:
    print('Did not find spec in any of these locations:', spec_locations)

space.clear_pipelines()

# A baseline metric
pl = waterline.pipeline.Pipeline("baseline")
pl.add_stage(OptStage(['-O3']), name="Optimize")
pl.add_stage(AlaskaBaselineStage(), name="Baseline")
pl.set_linker(AlaskaLinker())
space.add_pipeline(pl)


# Run Alaska in it's default configuration
pl = waterline.pipeline.Pipeline("alaska")
pl.add_stage(OptStage(['-O3']), name="Optimize")
pl.add_stage(AlaskaStage(), name="Alaska")
pl.set_linker(AlaskaLinker())
space.add_pipeline(pl)


# pl = waterline.pipeline.Pipeline("nohoisting")
# pl.add_stage(OptStage(['-O3']), name="Optimize")
# pl.add_stage(AlaskaStage(['--disable-hoisting']), name="Alaska")
# pl.set_linker(AlaskaLinker())
# space.add_pipeline(pl)


# pl = waterline.pipeline.Pipeline("gcc-suffering")
# pl.add_stage(OptStage(['-O3']), name="Optimize")
# pl.add_stage(AlaskaGCCSpecialCaseStage(['--disable-hoisting']), name="Alaska")
# pl.set_linker(AlaskaLinker())
# space.add_pipeline(pl)


# pl = waterline.pipeline.Pipeline("notracking")
# pl.add_stage(OptStage(['-O3']), name="Optimize")
# pl.add_stage(AlaskaStage(['--disable-tracking']), name="Alaska")
# pl.set_linker(AlaskaLinker())
# space.add_pipeline(pl)


# pl = waterline.pipeline.Pipeline("noinlining")
# pl.add_stage(OptStage(['-O3']), name="Optimize")
# pl.add_stage(AlaskaStage(['--disable-inlining']), name="Alaska")
# pl.set_linker(AlaskaLinker())
# space.add_pipeline(pl)


# res = space.run(runs=1, compile=False, run_name="gcc-pain", runner=PerfRunner())
res = space.run(runs=1, compile=True, run_name="test")
print(res)
