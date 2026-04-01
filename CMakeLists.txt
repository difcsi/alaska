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

alaska_option(ALASKA_SIZE_BITS 24)
alaska_option(CMAKE_BUILD_TYPE Release)
alaska_option_nodef(ALASKA_ENABLE_COMPONENTS "compiler;compiler-rts")

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
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -fPIC -g3 -Wno-unknown-warning-option -Wno-covered-switch-default ")
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && make -C ${CMAKE_SOURCE_DIR}/memcached memcached"
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-noservice && ulimit -s unlimited && python3 -m benchmarks.figure7"
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

add_custom_command(
  OUTPUT ${ALASKA_FIG8_CSV}
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 8"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-noservice && ulimit -s unlimited && python3 -m benchmarks.figure8"
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

set(ALASKA_RSSTRACKER_SO ${CMAKE_SOURCE_DIR}/opt/alaska-anchorage/lib/librsstracker.so)

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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
  COMMAND ${CMAKE_COMMAND} -E echo "Generating data for figure 9"
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ALASKA_RESULTS_DIR}
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && ulimit -s unlimited && python3 -m redis.frag && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure9.py"
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && ulimit -s unlimited && python3 -m redis.config_sweep && python3 ${CMAKE_SOURCE_DIR}/plotgen/figure10.py"
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && make -C ${CMAKE_SOURCE_DIR}/redis redis"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && ulimit -s unlimited && python3 -m redis.frag_large"
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
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && make -C ${CMAKE_SOURCE_DIR}/memcached memcached"
  COMMAND /bin/bash -lc ". ${CMAKE_SOURCE_DIR}/venv/bin/activate && . ${CMAKE_SOURCE_DIR}/opt/enable-alaska-anchorage && ulimit -s unlimited && python3 -m memcached.ycsb"
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
  COMMAND ${CMAKE_INSTALL_PREFIX}/bin/alaska -O3 ${CMAKE_SOURCE_DIR}/test/sanity.c -o ${CMAKE_BINARY_DIR}/sanity
  COMMAND ${CMAKE_INSTALL_PREFIX}/bin/alaska     ${CMAKE_SOURCE_DIR}/test/sanity.c -o ${CMAKE_BINARY_DIR}/sanity
  COMMAND ${CMAKE_BINARY_DIR}/sanity
  COMMENT "Building and running sanity test"
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
