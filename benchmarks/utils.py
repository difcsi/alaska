import os

from pathlib import Path


# Benchmark-harness verbosity. The figure sweeps emit a line per benchmark per
# build config while compiling, which buries waterline's progress bars in
# scrollback. Keep that chatter off by default; set ALASKA_BENCH_VERBOSE=1 (or
# true/yes) to bring it back for debugging.
VERBOSE = os.environ.get("ALASKA_BENCH_VERBOSE", "").lower() in ("1", "true", "yes", "on")


def vprint(*args, **kwargs):
  """print() that stays silent unless ALASKA_BENCH_VERBOSE is set."""
  if VERBOSE:
    print(*args, **kwargs)


def _prepend_env_path(var_name, new_path):
  current = os.environ.get(var_name, "")
  parts = [p for p in current.split(":") if p]
  if new_path in parts:
    return
  os.environ[var_name] = f"{new_path}:{current}" if current else new_path


def activate_local_toolchain(repo_root=None):
  """Prefer repository-pinned LLVM/gllvm tools over system binaries."""
  if repo_root is None:
    repo_root = Path(__file__).resolve().parents[1]

  bin_paths = [
      repo_root / "opt" / "llvm" / "bin",
      repo_root / "opt" / "gllvm" / "bin",
  ]
  lib_paths = [
      repo_root / "opt" / "llvm" / "lib",
      repo_root / "opt" / "gllvm" / "lib",
  ]

  vprint("Activating local toolchain in ", repo_root)

  for path in bin_paths:
    if path.is_dir():
      _prepend_env_path("PATH", str(path))

  for path in lib_paths:
    if path.is_dir():
      _prepend_env_path("LD_LIBRARY_PATH", str(path))
      # Ensure linker-time lookup for -lomp and other LLVM-provided libs.
      _prepend_env_path("LIBRARY_PATH", str(path))

  llvm_bin = repo_root / "opt" / "llvm" / "bin"
  if llvm_bin.is_dir():
    os.environ["LLVM_COMPILER_PATH"] = str(llvm_bin)

# The benchmark sweep configurations. Each name in ALASKA_BUILD_CONFIGS has a
# matching install at opt/alaska-<name> produced by build.sh, with its own compiler
# driver (alaska-transform / alaska-config) and runtime (libalaska). The benchmark
# harness compiles + links each benchmark once per config by pointing at that
# install's bin/lib, so a single sweep produces a `config` column with all of them.
#
# "baseline" (plain bundled clang, no instrumentation) is realized via
# `alaska-transform --baseline`, which skips every Alaska pass; it borrows any one
# install's driver (features are irrelevant in baseline mode) -- see BASELINE_DRIVER.
ALASKA_BUILD_CONFIGS = [
    "noservice",
    "anchorage",
    "refcount",
    "refcount-gc",
    "refcount-gc-anchorage",
]

BASELINE_CONFIG = "baseline"
# Which install's driver to borrow for baseline compilation (any works).
BASELINE_DRIVER = "noservice"


def alaska_root(repo_root=None):
  """Absolute path to the alaska repository root."""
  if repo_root is None:
    repo_root = Path(__file__).resolve().parents[1]
  return Path(repo_root)


def config_prefix(install, repo_root=None):
  """Install prefix (opt/alaska-<install>) for a build config."""
  return alaska_root(repo_root) / "opt" / f"alaska-{install}"


def get_spec_size():
  size = os.getenv("SPEC_SIZE")

  if size:
    if size in ['test', 'train', 'ref']:
      return size
    else:
      print(f'{size} is not one of test,train,ref')

  print("Using SPEC size 'ref'")
  return 'ref'

spec_locations = [
    "./SPEC2017.tar.gz",
    "~/SPEC2017.tar.gz",
    "/SPEC2017.tar.gz",
]
def find_spec():
    vprint('looking for SPEC in these locations:', spec_locations)
    for loc in spec_locations:
        loc = os.path.expanduser(loc)
        if os.path.isfile(loc):
            return str(Path(loc).resolve())
    return None
