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

export LLVM_RESOURCE_DIR=$(${ROOT}/opt/llvm/bin/clang -print-resource-dir)

mkdir -p opt

for config in noservice anchorage; do
  INSTALL_DIR=${ROOT}/opt/alaska-${config}

  buildstep "configure ${config}" cmake --preset ${config} -S $ROOT
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
