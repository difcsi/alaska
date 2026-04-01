#!/usr/bin/env bash

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

buildstep() {
  NAME=$1
  shift
  printf "\e[32m${NAME}\e[0m\n"
  # "$@" 2>&1 | sed $'s|^|\x1b[32m['"${NAME}"$']\x1b[39m |' || exit 1
  "$@"
}

# Compile llvm, and gclang.
buildstep "pull toolchain" tools/build_deps.sh

source $ROOT/opt/enable-toolchain

LLVM_LIBCXX_DIR=${ROOT}/opt/llvm/lib/x86_64-unknown-linux-gnu
LLVM_RESOURCE_DIR=$(${ROOT}/opt/llvm/bin/clang -print-resource-dir)
LLVM_LIBCXX_FLAGS="-stdlib=libc++"
LLVM_LIBCXX_LINK_FLAGS="-stdlib=libc++ -L${LLVM_LIBCXX_DIR} -Wl,-rpath,${LLVM_LIBCXX_DIR}"
LLVM_CXX_STANDARD_INCLUDES="${ROOT}/opt/llvm/include/c++/v1;${ROOT}/opt/llvm/include/x86_64-unknown-linux-gnu/c++/v1;${LLVM_RESOURCE_DIR}/include;/usr/local/include;/usr/include/x86_64-linux-gnu;/usr/include"


mkdir -p build

for config in noservice anchorage; do
  mkdir -p build/${config}

  pushd build/${config} 2>/dev/null

    INSTALL_DIR=${ROOT}/opt/alaska-${config}

    buildstep "configure ${config}" \
      cmake -S $ROOT -B . \
        -DCMAKE_INSTALL_PREFIX:PATH=${INSTALL_DIR} \
        -DCMAKE_C_COMPILER:FILEPATH=${ROOT}/opt/llvm/bin/clang \
        -DCMAKE_CXX_COMPILER:FILEPATH=${ROOT}/opt/llvm/bin/clang++ \
        -DCMAKE_CXX_FLAGS:STRING="${LLVM_LIBCXX_FLAGS}" \
        -DCMAKE_EXE_LINKER_FLAGS:STRING="${LLVM_LIBCXX_LINK_FLAGS}" \
        -DCMAKE_SHARED_LINKER_FLAGS:STRING="${LLVM_LIBCXX_LINK_FLAGS}" \
        -DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES:STRING="${LLVM_CXX_STANDARD_INCLUDES}" \
        -DALASKA_ENABLE_TESTING=OFF \
        -DALASKA_CONFIG=$ROOT/configs/config.${config}.cmake

    buildstep "build ${config}" make -j $(nproc) install

    # Create an enable file which can be sourced in bash
    ENABLE=$ROOT/opt/enable-alaska-${config}
    echo "source ${ROOT}/opt/enable-toolchain" > $ENABLE
    # Binary path
    echo "export PATH=$INSTALL_DIR/bin:\$PATH" >> $ENABLE
    # Library path
    echo "export LD_LIBRARY_PATH=$INSTALL_DIR/lib:\$LD_LIBRARY_PATH" >> $ENABLE
    # add a variable to tell where alaska is installed
    echo "export ALASKA=$INSTALL_DIR" >> $ENABLE
  popd 2>/dev/null
done
