#!/usr/bin/env bash

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

# Compile llvm, and gclang.
buildstep "pull toolchain" tools/build_deps.sh

source $ROOT/opt/enable-toolchain

export LLVM_RESOURCE_DIR=$(${ROOT}/opt/llvm/bin/clang -print-resource-dir)

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
for config in noservice anchorage refcount refcount-anchorage refcount-gc refcount-gc-anchorage; do
  INSTALL_DIR=${ROOT}/opt/alaska-${config}

  buildstep "configure ${config}" cmake --preset ${config} -S $ROOT "${EXTRA_CMAKE_ARGS[@]}"
  buildstep "build ${config}" cmake --build --preset ${config} --target install -j $(nproc)

  # Create an enable file which can be sourced in bash
  ENABLE=$ROOT/opt/enable-alaska-${config}
  echo "source ${ROOT}/opt/enable-toolchain" > $ENABLE
  # Binary path
  echo "export PATH=$INSTALL_DIR/bin:\$PATH" >> $ENABLE
  # Library path
  echo "export LD_LIBRARY_PATH=$INSTALL_DIR/lib:\$LD_LIBRARY_PATH" >> $ENABLE
  # add a variable to tell where alaska is installed
  echo "export ALASKA=$INSTALL_DIR" >> $ENABLE
done
