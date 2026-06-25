string(ASCII 27 Esc)
set(Purple "${Esc}[35m")
set(Green "${Esc}[32m")
set(Red "${Esc}[31m")
set(ColorReset "${Esc}[m")

function(alaska_switch OPT DEFAULT)
  set(${OPT} ${DEFAULT} CACHE BOOL "")

  if(${OPT} MATCHES "ON|On|on|TRUE|true|1")
    add_compile_definitions(${OPT}=1)
    # set(${OPT} ON PARENT_SCOPE)
    message(STATUS "${Purple}OPTION${ColorReset} ${OPT} ${Green}ON${ColorReset}")
  elseif(${OPT} MATCHES "OFF|Off|off|FALSE|false|0")
    set(${OPT} OFF PARENT_SCOPE)
    message(STATUS "${Purple}OPTION${ColorReset} ${OPT} ${Red}OFF${ColorReset}")
  else()
    message(FATAL_ERROR "${OPT} must be either ON or OFF")
  endif()
endfunction()


function(alaska_option OPT DEFAULT)
  set(${OPT} ${DEFAULT} CACHE STRING "")
  add_compile_definitions(${OPT}=${${OPT}})
  message(STATUS "${Purple}OPTION${ColorReset} ${OPT} ${Green}${${OPT}}${ColorReset}")
endfunction()

function(alaska_option_nodef OPT DEFAULT)
  set(${OPT} ${DEFAULT} CACHE STRING "")
  message(STATUS "${Purple}OPTION${ColorReset} ${OPT} ${Green}${${OPT}}${ColorReset}")
endfunction()


alaska_switch(ALASKA_ENABLE_COMPILER ON)
alaska_switch(ALASKA_ENABLE_TESTING  ON)
alaska_switch(ALASKA_CORE_ONLY       OFF)
alaska_switch(ALASKA_HTLB_SIM        OFF)

alaska_switch(ALASKA_YUKON           OFF)

# The three runtime services are independently gated so the benchmark presets can
# build any combination (see CMakePresets.json / configs). Per-config values are
# supplied by the preset's cacheVariables, which land in the cache *before* these
# switches run, so alaska_switch picks them up and emits the right -D.
#
#   ALASKA_ENABLE_REFCOUNT          reference counting: inc/dec passes, refcount
#                                   storage, nullcount map, zero-refcount stackscan
#                                   reclamation on the barrier thread.
#   ALASKA_ENABLE_CYCLE_COLLECTION  Anchorage's Bacon & Rajan cycle collector (GC).
#                                   Requires ALASKA_ENABLE_REFCOUNT.
#   ALASKA_ENABLE_ANCHORAGE         heap defragmentation (in-barrier compaction).
#
# Defaults are ON so the bare `default` preset still builds the full stack.
alaska_switch(ALASKA_ENABLE_REFCOUNT          ON)
alaska_switch(ALASKA_ENABLE_CYCLE_COLLECTION  ON)
alaska_switch(ALASKA_ENABLE_ANCHORAGE         ON)

# Measurement-only event counters (incref/decref/gc-free/compaction tallies).
alaska_switch(ALASKA_ENABLE_EVENT_COUNTERS    OFF)

# Cache-miss characterization probe: a simulated direct-mapped cache + distinct
# handle-table-line bitmap driven from the single refcount-mutation point
# (Mapping::inc_refcount/dec_refcount). Approximates Mapping-line miss rate and
# working set without perf (unavailable on this kernel). Heavy and perturbs
# timing -- characterization builds only, never timing runs. Built via ALASKA_PROBE=1.
alaska_switch(ALASKA_ENABLE_CACHE_PROBE       OFF)

# Cycle collection reads refcounts; it cannot be built without reference counting.
if(ALASKA_ENABLE_CYCLE_COLLECTION AND NOT ALASKA_ENABLE_REFCOUNT)
  message(FATAL_ERROR "ALASKA_ENABLE_CYCLE_COLLECTION requires ALASKA_ENABLE_REFCOUNT")
endif()

# ALASKA_SIZE_BITS is the width of the in-handle offset field, which also dictates
# the handle-table base address (see runtime/include/alaska/Configuration.hpp).
# A service config (included below) may override it, so establish the default
# WITHOUT emitting the -D here -- we define it exactly once, after the config, to
# avoid a duplicate/conflicting -DALASKA_SIZE_BITS on the compile command line.
alaska_option_nodef(ALASKA_SIZE_BITS 24)
alaska_option(CMAKE_BUILD_TYPE Release)
alaska_option_nodef(ALASKA_ENABLE_COMPONENTS "compiler;compiler-rts")

# A service configuration (configs/config.<service>.cmake) selects the allocator
# service and may override tunables such as ALASKA_SIZE_BITS. The active config is
# chosen by the CMake preset via -DALASKA_CONFIG=...; the bare "default" preset
# sets none, in which case the defaults above stand.
if(ALASKA_CONFIG)
  if(NOT EXISTS "${ALASKA_CONFIG}")
    message(FATAL_ERROR "ALASKA_CONFIG does not exist: ${ALASKA_CONFIG}")
  endif()
  message(STATUS "${Purple}CONFIG${ColorReset} ${Green}${ALASKA_CONFIG}${ColorReset}")
  include("${ALASKA_CONFIG}")
endif()

# Define the handle-size macro once, from the final (possibly config-overridden) value.
add_compile_definitions(ALASKA_SIZE_BITS=${ALASKA_SIZE_BITS})

# Bytes reserved at the end of every sized backing allocation for a liballocs
# trailing `struct insert` (per-object lifetime-policy mask). The stackscan build
# injects -DALASKA_LIBALLOCS_INSERT_RESERVE=8 (== sizeof(struct insert)); standalone
# Alaska leaves it 0 (no reservation, no behavior change). See ThreadCache.cpp.
if(NOT DEFINED ALASKA_LIBALLOCS_INSERT_RESERVE)
  set(ALASKA_LIBALLOCS_INSERT_RESERVE 0)
endif()
add_compile_definitions(ALASKA_LIBALLOCS_INSERT_RESERVE=${ALASKA_LIBALLOCS_INSERT_RESERVE})
message(STATUS "ALASKA_LIBALLOCS_INSERT_RESERVE=${ALASKA_LIBALLOCS_INSERT_RESERVE}")

set(ALASKA_VERSION 2)


set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)


# Check if the build type is Release
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(ALASKA_ENABLE_LOGGING=1)
endif()



list(FIND ALASKA_ENABLE_COMPONENTS "compiler" INDEX)
if (NOT INDEX EQUAL -1)
  message(STATUS "compiler")
endif()


list(FIND ALASKA_ENABLE_COMPONENTS "compiler-rts" INDEX)
if (NOT INDEX EQUAL -1)
  message(STATUS "compiler-rts")
endif()


# -------------------------------------------------------------------------------------------

# The bundled clang defaults to the newest installed GCC's libstdc++, which may be
# too new for it to parse (e.g. clang 16 cannot compile against libstdc++ 16). Pin
# clang to an older GCC's libstdc++/headers via --gcc-install-dir. Auto-detected as
# the newest GCC install dir with major version <= 13 (the highest clang 16 handles
# reliably). Override with -DALASKA_GCC_INSTALL_DIR=/path, or set it to OFF to skip.
# This must run before project() so the C/C++ implicit-include probe (which is later
# frozen into CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES) resolves the older headers.
if(CMAKE_CXX_COMPILER MATCHES "clang")
  if(NOT DEFINED ALASKA_GCC_INSTALL_DIR)
    file(GLOB _alaska_gcc_dirs LIST_DIRECTORIES true /usr/lib/gcc/*/*)
    set(_alaska_gcc_best "")
    set(_alaska_gcc_best_ver 0)
    foreach(_d ${_alaska_gcc_dirs})
      if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/crtbegin.o")
        get_filename_component(_v "${_d}" NAME)
        if(_v MATCHES "^[0-9]+$" AND NOT _v GREATER 13 AND _v GREATER _alaska_gcc_best_ver)
          set(_alaska_gcc_best_ver ${_v})
          set(_alaska_gcc_best "${_d}")
        endif()
      endif()
    endforeach()
    set(ALASKA_GCC_INSTALL_DIR "${_alaska_gcc_best}" CACHE STRING
      "GCC install dir clang uses for libstdc++ (--gcc-install-dir); set OFF to disable")
  endif()
  if(ALASKA_GCC_INSTALL_DIR AND NOT ALASKA_GCC_INSTALL_DIR STREQUAL "OFF")
    message(STATUS "Pinning clang libstdc++ to GCC install dir: ${ALASKA_GCC_INSTALL_DIR}")
    set(_alaska_gcc_flag "--gcc-install-dir=${ALASKA_GCC_INSTALL_DIR}")
    set(CMAKE_C_FLAGS            "${CMAKE_C_FLAGS} ${_alaska_gcc_flag}")
    set(CMAKE_CXX_FLAGS          "${CMAKE_CXX_FLAGS} ${_alaska_gcc_flag}")
    set(CMAKE_EXE_LINKER_FLAGS   "${CMAKE_EXE_LINKER_FLAGS} ${_alaska_gcc_flag}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_alaska_gcc_flag}")
  else()
    message(WARNING "No suitable GCC (<=13) found for clang's libstdc++; C++ host tools may fail to build")
  endif()
endif()

cmake_minimum_required(VERSION 3.13)
project(Alaska C CXX ASM)


if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(STATUS "Using Clang")
else()
  message(STATUS "Not using Clang. Disabling compilation of the Alaska Compiler because we cannot produce bitcode files with the current compiler")
  set(ALASKA_ENABLE_COMPILER FALSE)
endif()

add_compile_definitions("ALASKA_INSTALL_PREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
add_definitions(-include "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/alaska/config.h")

set(CMAKE_INSTALL_LIBDIR ${CMAKE_INSTALL_PREFIX}/lib)
set(CMAKE_INSTALL_BINDIR ${CMAKE_INSTALL_PREFIX}/bin)
set(CMAKE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_PREFIX}/include)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -fPIC -g3 -Wno-unknown-warning-option -Wno-covered-switch-default ")
set(CMAKE_INSTALL_MESSAGE LAZY)
set(CMAKE_VERBOSE_MAKEFILE OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")
include(LLVMIRUtil)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
# https://discourse.nixos.org/t/get-clangd-to-find-standard-headers-in-nix-shell/11268/10
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  if(NOT CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES)
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
  endif()
endif(CMAKE_EXPORT_COMPILE_COMMANDS)


find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()



add_subdirectory(runtime)


if(ALASKA_ENABLE_COMPILER)
  # configure LLVM
  find_package(LLVM 16 REQUIRED CONFIG)

  set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/)
  set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/)


  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  include(HandleLLVMOptions)
  include(AddLLVM)
  add_subdirectory(compiler)
endif(ALASKA_ENABLE_COMPILER)

set(ALASKA_RESULTS_DIR ${CMAKE_SOURCE_DIR}/results)
set(ALASKA_FIG7_CSV ${ALASKA_RESULTS_DIR}/figure7.csv)
set(ALASKA_FIG7_PDF ${ALASKA_RESULTS_DIR}/figure7.pdf)
set(ALASKA_FIG_COMPILE_PDF ${ALASKA_RESULTS_DIR}/figure_compile.pdf)
set(ALASKA_FIG8_CSV ${ALASKA_RESULTS_DIR}/figure8.csv)
set(ALASKA_FIG8_PDF ${ALASKA_RESULTS_DIR}/figure8.pdf)
set(ALASKA_REDIS_FIG9_PDF ${ALASKA_RESULTS_DIR}/figure9.pdf)
set(ALASKA_REDIS_FIG10_PDF ${ALASKA_RESULTS_DIR}/figure10.pdf)
set(ALASKA_REDIS_LARGE_CSV ${ALASKA_RESULTS_DIR}/redis-alaska-large.csv)
set(ALASKA_REDIS_FIG11_PDF ${ALASKA_RESULTS_DIR}/figure11.pdf)
set(ALASKA_MEMCACHED_SWEEP_CSV ${ALASKA_RESULTS_DIR}/memcached-sweep.csv)
set(ALASKA_MEMCACHED_FIG12_PDF ${ALASKA_RESULTS_DIR}/figure12.pdf)
set(ALASKA_VENV_TOUCHFILE ${CMAKE_SOURCE_DIR}/venv/touchfile)

# venv setup
add_custom_command(
  OUTPUT ${ALASKA_VENV_TOUCHFILE}
  COMMAND /bin/bash -lc "test -d ${CMAKE_SOURCE_DIR}/venv || virtualenv ${CMAKE_SOURCE_DIR}/venv"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && pip install -Ur ${CMAKE_SOURCE_DIR}/requirements.txt && touch ${ALASKA_VENV_TOUCHFILE}"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/requirements.txt
  USES_TERMINAL
  VERBATIM
)
add_custom_target(venv DEPENDS ${ALASKA_VENV_TOUCHFILE})

# Build redis w/ anchorage
set(ALASKA_REDIS_SERVER_ALASKA ${CMAKE_SOURCE_DIR}/redis/bin/redis-server-alaska)
add_custom_command(
  OUTPUT ${ALASKA_REDIS_SERVER_ALASKA}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${CMAKE_SOURCE_DIR}/redis/Makefile
  USES_TERMINAL
  VERBATIM
)
add_custom_target(redis_alaska DEPENDS ${ALASKA_REDIS_SERVER_ALASKA})

# Build memcached w/ anchorage
set(ALASKA_MEMCACHED_ALASKA ${CMAKE_SOURCE_DIR}/memcached/bin/memcached-alaska)
add_custom_command(
  OUTPUT ${ALASKA_MEMCACHED_ALASKA}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && make -C ${CMAKE_SOURCE_DIR}/memcached memcached"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${CMAKE_SOURCE_DIR}/memcached/Makefile
  USES_TERMINAL
  VERBATIM
)
add_custom_target(memcached_alaska DEPENDS ${ALASKA_MEMCACHED_ALASKA})

add_custom_command(
  OUTPUT ${ALASKA_FIG7_CSV}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 7"
  # The harness sweeps every build config itself (it points at each opt/alaska-<config>
  # install by absolute path), so all five Alaska installs + opt/enable-toolchain must
  # already exist (run build.sh first). We source only the toolchain here, not a single
  # alaska install.
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-toolchain && ulimit -s unlimited && python3 -m benchmarks.figure7"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/bench/results/figure7/all.csv ${ALASKA_FIG7_CSV}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${CMAKE_SOURCE_DIR}/benchmarks/figure7.py
    ${CMAKE_SOURCE_DIR}/plotgen/figure7.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure7_csv DEPENDS ${ALASKA_FIG7_CSV})

add_custom_command(
  OUTPUT ${ALASKA_FIG7_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating figure 7"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure7.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_FIG7_CSV}
    ${CMAKE_SOURCE_DIR}/plotgen/figure7.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure7_pdf DEPENDS ${ALASKA_FIG7_PDF})

# Compile-time overhead figure. The figure-7 data run also writes
# bench/results/figure7/compile_times.csv (via TimedStage), so depend on the
# figure-7 CSV to guarantee the sweep ran, then plot the compile-time chart.
add_custom_command(
  OUTPUT ${ALASKA_FIG_COMPILE_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating compile-time figure"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure_compile.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_FIG7_CSV}
    ${CMAKE_SOURCE_DIR}/plotgen/figure_compile.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure_compile_pdf DEPENDS ${ALASKA_FIG_COMPILE_PDF})

add_custom_command(
  OUTPUT ${ALASKA_FIG8_CSV}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 8"
  # As with figure 7, the harness selects each install internally; source only the toolchain.
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-toolchain && ulimit -s unlimited && python3 -m benchmarks.figure8"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/bench/results/figure8/all.csv ${ALASKA_FIG8_CSV}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${CMAKE_SOURCE_DIR}/benchmarks/figure8.py
    ${CMAKE_SOURCE_DIR}/plotgen/figure8.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure8_csv DEPENDS ${ALASKA_FIG8_CSV})

add_custom_command(
  OUTPUT ${ALASKA_FIG8_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating figure 8"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure8.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_FIG8_CSV}
    ${ALASKA_FIG7_CSV}
    ${CMAKE_SOURCE_DIR}/plotgen/figure8.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure8_pdf DEPENDS ${ALASKA_FIG8_PDF})

# The redis/memcached fragmentation figures exercise the full Alaska stack (heap
# defragmentation alongside reference counting + cycle collection), which is the
# refcount-gc-anchorage configuration -- the bare `anchorage` config is now
# compaction-only. The RSS tracker .so is staged in that install's lib.
set(ALASKA_RSSTRACKER_SO ${CMAKE_SOURCE_DIR}/opt/alaska-refcount-gc-anchorage/lib/librsstracker.so)

add_custom_command(
  OUTPUT ${ALASKA_RSSTRACKER_SO}
  COMMAND cc -shared -fPIC -pthread
          -o ${ALASKA_RSSTRACKER_SO}
          ${CMAKE_SOURCE_DIR}/runtime/extra/rsstracker.c
          -lpthread
  DEPENDS ${CMAKE_SOURCE_DIR}/runtime/extra/rsstracker.c
  COMMENT "Building librsstracker.so"
  VERBATIM
)

add_custom_target(rsstracker DEPENDS ${ALASKA_RSSTRACKER_SO})

add_custom_command(
  OUTPUT ${ALASKA_REDIS_FIG9_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Compiling redis"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 9"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && ulimit -s unlimited && python3 -m redis.frag && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure9.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_RSSTRACKER_SO}
    ${CMAKE_SOURCE_DIR}/redis/Makefile
    ${CMAKE_SOURCE_DIR}/redis/frag.py
    ${CMAKE_SOURCE_DIR}/plotgen/figure9.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure9_pdf DEPENDS ${ALASKA_REDIS_FIG9_PDF})

add_custom_command(
  OUTPUT ${ALASKA_REDIS_FIG10_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 10"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && ulimit -s unlimited && python3 -m redis.config_sweep && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure10.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_REDIS_FIG9_PDF}
    ${CMAKE_SOURCE_DIR}/redis/config_sweep.py
    ${CMAKE_SOURCE_DIR}/plotgen/figure10.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure10_pdf DEPENDS ${ALASKA_REDIS_FIG10_PDF})

add_custom_command(
  OUTPUT ${ALASKA_REDIS_LARGE_CSV}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 11"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && ulimit -s unlimited && python3 -m redis.frag_large"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_RSSTRACKER_SO}
    ${CMAKE_SOURCE_DIR}/redis/Makefile
    ${CMAKE_SOURCE_DIR}/redis/frag_large.py
    ${CMAKE_SOURCE_DIR}/redis/bin/redis-server-alaska
  USES_TERMINAL
  VERBATIM
)

add_custom_target(redis_alaska_large_csv DEPENDS ${ALASKA_REDIS_LARGE_CSV})

add_custom_command(
  OUTPUT ${ALASKA_REDIS_FIG11_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Plotting figure 11"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure9.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_REDIS_LARGE_CSV}
    ${CMAKE_SOURCE_DIR}/plotgen/figure9.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure11_pdf DEPENDS ${ALASKA_REDIS_FIG11_PDF})

add_custom_command(
  OUTPUT ${ALASKA_MEMCACHED_SWEEP_CSV}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 12"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && make -C ${CMAKE_SOURCE_DIR}/memcached memcached"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-refcount-gc-anchorage && ulimit -s unlimited && python3 -m memcached.ycsb"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${CMAKE_SOURCE_DIR}/memcached/Makefile
    ${CMAKE_SOURCE_DIR}/memcached/ycsb.py
    ${CMAKE_SOURCE_DIR}/memcached/bin/memcached-alaska
  USES_TERMINAL
  VERBATIM
)

add_custom_target(memcached_sweep_csv DEPENDS ${ALASKA_MEMCACHED_SWEEP_CSV})

add_custom_command(
  OUTPUT ${ALASKA_MEMCACHED_FIG12_PDF}
  COMMAND ${CMAKE_COMMAND} -E echo "Plotting figure 12"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure12.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    ${ALASKA_VENV_TOUCHFILE}
    ${ALASKA_MEMCACHED_SWEEP_CSV}
    ${CMAKE_SOURCE_DIR}/plotgen/figure12.py
  USES_TERMINAL
  VERBATIM
)

add_custom_target(figure12_pdf DEPENDS ${ALASKA_MEMCACHED_FIG12_PDF})
# ---------------------------------------------------------------------------
# Convenience targets (mirrors of the former Makefile)
# ---------------------------------------------------------------------------

# sanity: build and run test/sanity.c with the installed alaska compiler
add_custom_target(sanity
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/sanity.c -o ${CMAKE_BINARY_DIR}/sanity
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska     ${CMAKE_SOURCE_DIR}/test/sanity.c -o ${CMAKE_BINARY_DIR}/sanity
  COMMAND ${CMAKE_BINARY_DIR}/sanity
  COMMENT "Building and running sanity test"
)

# heap-promote: exercise the Yukon stack-to-heap promotion pass in all three
# modes (off / promote-all / heap-hints). The program self-checks and exits
# non-zero on mismatch, so any divergence fails the target.
add_custom_target(heap-promote
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/heap_hints.c -o ${CMAKE_BINARY_DIR}/heap_hints_off
  COMMAND ${CMAKE_BINARY_DIR}/heap_hints_off
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 --stack-promote ${CMAKE_SOURCE_DIR}/test/heap_hints.c -o ${CMAKE_BINARY_DIR}/heap_hints_all
  COMMAND ${CMAKE_BINARY_DIR}/heap_hints_all
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 --heap-hints ${CMAKE_SOURCE_DIR}/test/heap_hints.txt ${CMAKE_SOURCE_DIR}/test/heap_hints.c -o ${CMAKE_BINARY_DIR}/heap_hints_hinted
  COMMAND ${CMAKE_BINARY_DIR}/heap_hints_hinted
  COMMENT "Building and running the stack-to-heap promotion test (off/all/hinted)"
)

# keep-raw: exercise the keep-raw optimization for non-escaping malloc/calloc.
# Keep-raw is ON by default; the test also builds a --no-keep-raw baseline. The
# program self-checks and exits non-zero on mismatch, so any divergence between
# the two builds fails the target -- keeping non-escaping objects raw must be
# transparent.
add_custom_target(keep-raw
  COMMAND bash ${CMAKE_SOURCE_DIR}/test/keep_raw_events.sh ${CMAKE_INSTALL_PREFIX}/bin/alaska ${CMAKE_SOURCE_DIR}/test/keep_raw.c ${CMAKE_BINARY_DIR}
  COMMENT "Building and running the keep-raw non-escaping malloc test (off/on)"
)

# keep-raw-events: build keep_raw.c off/on and assert (via the runtime event
# counters) that the keep-raw build performs strictly fewer hallocs with
# identical output. SKIPS the halloc-delta check on a non-measurement build
# (event counters off) rather than failing. Needs ALASKA_MEASURE=1 to be useful.
add_custom_target(keep-raw-events
  COMMAND bash ${CMAKE_SOURCE_DIR}/test/keep_raw_events.sh ${CMAKE_INSTALL_PREFIX}/bin/alaska ${CMAKE_SOURCE_DIR}/test/keep_raw.c ${CMAKE_BINARY_DIR}
  COMMENT "Comparing halloc counts with/without keep-raw (needs ALASKA_MEASURE=1 build)"
)

# refcount-collect: integration test that a handle with no references is
# reclaimed automatically (in bounded time) by the background barrier thread,
# with no explicit hfree and no forced barrier. The program self-checks and
# exits non-zero if the collector does not free the garbage in time.
add_custom_target(refcount-collect
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_collect.c -o ${CMAKE_BINARY_DIR}/refcount_collect
  COMMAND ${CMAKE_BINARY_DIR}/refcount_collect
  COMMENT "Building and running the refcount auto-reclamation integration test"
)

# refcount-collect-mt: the multithreaded counterpart. Several producer threads
# create+drop handles and then park, alive, while the background collector
# reclaims their garbage -- so the stop-the-world barrier must coordinate every
# thread, not just main. Self-checks and exits non-zero on failure.
add_custom_target(refcount-collect-mt
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_collect_mt.c -o ${CMAKE_BINARY_DIR}/refcount_collect_mt
  COMMAND ${CMAKE_BINARY_DIR}/refcount_collect_mt
  COMMENT "Building and running the multithreaded refcount auto-reclamation integration test"
)

# refcount-collect-cycle: integration test that a heap *cycle* of handles that
# reference only each other -- which plain reference counting can never reclaim --
# is collected automatically by Anchorage's background cycle collector, with no
# explicit hfree and no forced barrier. Self-checks and exits non-zero on failure.
add_custom_target(refcount-collect-cycle
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_collect_cycle.c -o ${CMAKE_BINARY_DIR}/refcount_collect_cycle
  COMMAND ${CMAKE_BINARY_DIR}/refcount_collect_cycle
  COMMENT "Building and running the cycle-collection auto-reclamation integration test"
)

# refcount-reclaim-reuse: regression test for a handle-reuse fault in the reclamation
# path -- an explicitly hfree'd refcount-0 handle was left in the GC nullcount set, so a
# later allocation that recycled its mapping slot could be reclaimed while live. Builds
# and explicitly tears down linked lists across rounds (recycling slots while the
# background collector runs) and self-checks integrity; exits non-zero on corruption.
add_custom_target(refcount-reclaim-reuse
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_reclaim_reuse.c -o ${CMAKE_BINARY_DIR}/refcount_reclaim_reuse
  COMMAND ${CMAKE_BINARY_DIR}/refcount_reclaim_reuse
  COMMENT "Building and running the refcount reclamation handle-reuse regression test"
)

# refcount-elision: soundness gate for the compiler's reference-count barrier
# elisions (Tier 1 non-handle stores, Tier 2 self-copies). Builds linked lists that
# mix real child handles (whose inc must be kept) with non-handle fields (whose inc
# is elided), churns the allocator to reuse any wrongly-freed slot, and re-verifies
# integrity. The program self-checks and exits non-zero on any corruption / premature
# free / leak / crash. (It uses the COMPILER-inserted store barriers, so it exercises
# the elision passes themselves.)
add_custom_target(refcount-elision
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_elision.c -o ${CMAKE_BINARY_DIR}/refcount_elision
  COMMAND ${CMAKE_BINARY_DIR}/refcount_elision
  COMMENT "Building and running the refcount barrier-elision soundness test"
)

# refcount-huge-children: regression test for huge-object dec-on-free. A huge container
# (no Mapping -- the raw backing pointer is returned) full of handles used to leak its
# children's refcounts on free because the dec-on-free scan keyed its base/size on the
# Mapping. Fills a huge handle array, frees it, and checks each child (kept live by a
# second array) dropped exactly one reference. Exits non-zero if any child was not dec'd.
add_custom_target(refcount-huge-children
  COMMAND ${CMAKE_COMMAND} -E env "LDFLAGS=-fuse-ld=bfd $ENV{LDFLAGS}" ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/refcount_huge_children.c -o ${CMAKE_BINARY_DIR}/refcount_huge_children
  COMMAND ${CMAKE_BINARY_DIR}/refcount_huge_children
  COMMENT "Building and running the huge-object dec-on-free regression test"
)

# unit: run the compiler unit-test suite via tools/unittest.py
add_custom_target(unit
  COMMAND ${CMAKE_SOURCE_DIR}/tools/unittest.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running compiler unit tests"
)

# docs: generate Doxygen documentation
add_custom_target(docs
  COMMAND doxygen ${CMAKE_SOURCE_DIR}/Doxyfile
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating Doxygen documentation"
)

# docker: build and enter the Docker image
add_custom_target(docker
  COMMAND docker build -t alaska ${CMAKE_SOURCE_DIR}
  COMMAND docker run -it --rm alaska bash
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building and running Alaska Docker image"
)

# deps: fetch clang and gclang if not already present
add_custom_target(deps
  COMMAND ${CMAKE_SOURCE_DIR}/tools/get_llvm.sh
  COMMAND ${CMAKE_SOURCE_DIR}/tools/build_gclang.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Fetching dependencies (LLVM/clang and gclang)"
)

# redis: build the redis test under Nix
add_custom_target(redis
  COMMAND nix develop --command bash -c "source ${CMAKE_SOURCE_DIR}/enable && make -C ${CMAKE_SOURCE_DIR}/test/redis"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building redis test"
)

# mrproper: remove the entire build directory
add_custom_target(mrproper
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}
  COMMENT "Removing build directory (mrproper)"
)
