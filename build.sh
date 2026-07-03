#!/usr/bin/env bash

# PORT-NOTE: Ported from main-rc onto dev. Two families of change vs the original:
#   1. Toolchain bootstrap: main-rc bundled an `opt/llvm` toolchain provisioned by
#      `tools/build_deps.sh` and sourced via `opt/enable-toolchain`. dev instead
#      bootstraps clang+gclang under `./local` via `tools/get_llvm.sh` /
#      `tools/build_gclang.sh` and exposes it through the top-level `./enable`. All
#      toolchain paths below were retargeted from `opt/llvm` -> `local` accordingly.
#   2. Anchorage is FUNCTIONAL on dev: the ALASKA_ENABLE_ANCHORAGE gate wires dev's
#      existing Heap::compact_sizedpages() into the periodic barrier thread (see
#      runtime/rt/init.cpp), so the `anchorage`, `refcount-anchorage`,
#      `refcount-gc-anchorage`, and `refcount-gc-anchorage-defer` presets perform
#      real in-barrier heap compaction. Runtime kill-switch: ALASKA_NO_COMPACT=1.

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# Run a build step quietly: print only the step name on success, capturing the
# command's (often very chatty) output to a temp log. If the command fails, dump
# the captured output and abort with its exit code so failures are still debuggable.
buildstep() {
  NAME=$1
  shift
  printf "\e[32m%s\e[0m\n" "$NAME"
  local log
  log=$(mktemp)
  if "$@" >"$log" 2>&1; then
    rm -f "$log"
  else
    local rc=$?
    printf "\e[31m[%s] FAILED (exit %d):\e[0m\n" "$NAME" "$rc"
    cat "$log"
    rm -f "$log"
    exit "$rc"
  fi
}

# PORT-NOTE: dev provisions the toolchain under ./local instead of ./opt/llvm.
# `tools/get_llvm.sh` downloads the pinned clang+llvm release and installs the
# gllvm/gclang wrappers into ./local. `tools/build_gclang.sh` (re)builds just the
# gclang wrappers; note it requires a `.config` (`make menuconfig`) and is otherwise
# a no-op, since get_llvm.sh already provisions gclang -- it is invoked here for
# parity with dev's `make deps` target.
buildstep "pull toolchain (llvm)" tools/get_llvm.sh
buildstep "pull toolchain (gclang)" tools/build_gclang.sh

# PORT-NOTE: source dev's top-level ./enable (sets PATH/LD_LIBRARY_PATH to ./local)
# in place of main-rc's opt/enable-toolchain.
source $ROOT/enable

# PORT-NOTE: resource dir comes from the ./local clang, not ./opt/llvm/bin/clang.
export LLVM_RESOURCE_DIR=$(${ROOT}/local/bin/clang -print-resource-dir)

mkdir -p opt

# Measurement build: set ALASKA_MEASURE=1 to compile in the runtime event
# counters (incref/decref/gc-free/compaction tallies, see EventCounters.hpp) so a
# figure-7 run can count those events. OFF by default -- the timing builds carry
# no counter overhead. This reconfigures the same install dirs, so do a plain
# `./build.sh` afterwards to get back to clean timing builds.
#
# We pass the flag EXPLICITLY in both cases. CMake `set(... CACHE BOOL)` defaults do
# NOT override an existing cache entry, so once a measurement build wrote
# ALASKA_ENABLE_EVENT_COUNTERS=ON into a build dir's cache, a plain `./build.sh`
# would silently keep counters ON forever. Forcing =OFF here makes "measurement" a
# per-invocation choice driven solely by ALASKA_MEASURE.
EXTRA_CMAKE_ARGS=()
# The cache-miss characterization probe reports through the event-counter dump, so
# turning it on implies a measurement build.
if [ -n "${ALASKA_PROBE}" ]; then
  ALASKA_MEASURE=1
fi
if [ -n "${ALASKA_MEASURE}" ]; then
  EXTRA_CMAKE_ARGS+=("-DALASKA_ENABLE_EVENT_COUNTERS=ON")
  printf "\e[35m[measurement build: event counters ON]\e[0m\n"
else
  EXTRA_CMAKE_ARGS+=("-DALASKA_ENABLE_EVENT_COUNTERS=OFF")
fi
# Cache-miss characterization probe: set ALASKA_PROBE=1 to compile in the simulated
# cache / distinct-line instrumentation in the refcount hot path (see CMakeLists and
# EventCounters.cpp). It perturbs timing, so use it only for characterization, never
# for figure-7 timing runs. Forced explicitly in both cases for the same
# cache-stickiness reason as the event counters above.
if [ -n "${ALASKA_PROBE}" ]; then
  EXTRA_CMAKE_ARGS+=("-DALASKA_ENABLE_CACHE_PROBE=ON")
  printf "\e[35m[cache probe build: simulated-cache instrumentation ON]\e[0m\n"
else
  EXTRA_CMAKE_ARGS+=("-DALASKA_ENABLE_CACHE_PROBE=OFF")
fi

# The benchmark configurations. "baseline" (plain bundled clang) is realized in the
# benchmark harness via `alaska-transform --baseline` and needs no build of its own,
# so it is not in this list. The six Alaska variants below each get their own build
# dir, install prefix, and opt/enable-alaska-<config> script. Their feature triple
# (REFCOUNT / CYCLE_COLLECTION / ANCHORAGE) is set by the matching preset in
# CMakePresets.json.
# PORT-NOTE: the *-anchorage configs perform real in-barrier compaction on dev
# (Heap::compact_sizedpages wired into the barrier thread; ALASKA_NO_COMPACT to disable).
for config in noservice anchorage refcount refcount-anchorage refcount-gc refcount-gc-anchorage refcount-gc-anchorage-defer; do
  INSTALL_DIR=${ROOT}/opt/alaska-${config}

  buildstep "configure ${config}" cmake --preset ${config} -S $ROOT "${EXTRA_CMAKE_ARGS[@]}"
  buildstep "build ${config}" cmake --build --preset ${config} --target install -j $(nproc)

  # Create an enable file which can be sourced in bash
  ENABLE=$ROOT/opt/enable-alaska-${config}
  # PORT-NOTE: source dev's ./enable (was opt/enable-toolchain on main-rc).
  echo "source ${ROOT}/enable" > $ENABLE
  # Binary path
  echo "export PATH=$INSTALL_DIR/bin:\$PATH" >> $ENABLE
  # Library path
  echo "export LD_LIBRARY_PATH=$INSTALL_DIR/lib:\$LD_LIBRARY_PATH" >> $ENABLE
  # add a variable to tell where alaska is installed
  echo "export ALASKA=$INSTALL_DIR" >> $ENABLE
done
