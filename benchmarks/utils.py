import os

from pathlib import Path


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

  print("Activating local toolchain in ", repo_root)

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
    print('looking for SPEC in these locations:', spec_locations)
    for loc in spec_locations:
        loc = os.path.expanduser(loc)
        if os.path.isfile(loc):
            return str(Path(loc).resolve())
    return None
