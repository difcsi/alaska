#!/usr/bin/env bash

# SOURCE=test/list.c
# SOURCE=/tank/nick/alaska/bench/ir/SPEC2017/605.mcf_s/input.bc
# ARGS=$(pwd)/bench/src/SPEC2017/SPEC2017/benchspec/CPU/505.mcf_r/data/test/input/inp.in
#
SOURCE=test/lua/lua.bc
ARGS=test/nbody.lua

make -j

local/bin/alaska -O3 -b -k $SOURCE -o build/noprofile

ALASKA_PROFON=y local/bin/alaska -O3 -k $SOURCE -o build/profileBinary
build/profileBinary $ARGS # run the profiler

ALASKA_HPROF=$(pwd)/alaska.hprof local/bin/alaska -O3 -k $SOURCE -o build/profiled

hyperfine -M 3 "build/noprofile $ARGS" \
               "build/profiled $ARGS" \
               "build/noprofile.base $ARGS"
