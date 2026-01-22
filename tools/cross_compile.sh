#!/usr/bin/bash


# This script tests cross compilation (to riscv) for a simple test program


set -e

export PATH=/usr/lib/llvm-21/bin:$PATH


ROOT=$(pwd)


B=${ROOT}/build-cross
L=${B}/local

mkdir -p ${B}



mkdir -p ${B}/host
pushd ${B}/host
  export CC=clang-21
  export CXX=clang++-21

  echo "Building Alaska for host..."
  if [ ! -f Makefile ]; then
      cmake ../../ -DALASKA_ENABLE_TESTING=OFF -DCMAKE_INSTALL_PREFIX:PATH=${L}
  fi
  make -j 32 install
popd

mkdir -p ${B}/riscv
pushd ${B}/riscv
  echo "Building Alaska for RISC-V..."
  export RISCV=/opt/riscv
  export CC=$RISCV/bin/riscv64-unknown-linux-gnu-gcc
  export CXX=$RISCV/bin/riscv64-unknown-linux-gnu-g++
  export AS=$RISCV/bin/riscv64-unknown-linux-gnu-as
  export LD=$RISCV/bin/riscv64-unknown-linux-gnu-ld
  cmake ../../ \
      -DALASKA_REVISION="TEST" \
      -DALASKA_ENABLE_COMPILER=OFF \
      -DALASKA_ENABLE_TESTING=OFF \
      -DALASKA_ENABLE_LOGGING=OFF \
      -DALASKA_CORE_ONLY=OFF \
      -DALASKA_YUKON=OFF \
      -DALASKA_YUKON_NO_HARDWARE=ON \
      -DALASKA_CORE_BITCODE=OFF \
      -DALASKA_LIBRARY_TYPE=SHARED \
      -DCMAKE_BUILD_TYPE=Release \
      -DALASKA_SIZE_BITS=32 \
      -DCMAKE_SYSROOT=$RISCV/sysroot
  make -j 32
popd



CFLAGS="-target riscv64-unknown-linux-gnu --sysroot=/opt/riscv/sysroot --gcc-toolchain=/opt/riscv "
CFLAGS+="-g0 -fno-sanitize=cfi -O3 -DALASKA_SIZE_BITS=32 -include runtime/alaska/config.h -I./runtime "

# compile the translate.cpp for the host.
clang++ ${CFLAGS} runtime/rt/translate.cpp -c -emit-llvm -o ${B}/translate.bc


mkdir -p ${B}/stub
for f in runtime/stub/*.c; do
    name=$(basename ${f} .c)
    clang ${CFLAGS} ${f} -c -emit-llvm -o ${B}/stub/${name}.bc
done
llvm-link ${B}/stub/*.bc -o ${B}/stub.bc


opt --strip-debug ${B}/stub.bc -o ${B}/stub.bc
opt --strip-debug ${B}/translate.bc -o ${B}/translate.bc

llvm-dis ${B}/stub.bc -o ${B}/stub.ll
llvm-dis ${B}/translate.bc -o ${B}/translate.ll



SOURCE=test/list.c

bitcode=${B}/prog.bc
clang ${CFLAGS} ${SOURCE} -c -emit-llvm -o ${bitcode}
# link the stub
llvm-link ${bitcode} --internalize ${B}/stub.bc -o ${bitcode}

opt ${bitcode} -o ${bitcode} -passes=mergereturn,break-crit-edges,loop-simplify,lcssa,indvars,mem2reg,instnamer



baselinebc=${B}/baseline.bc
cp ${bitcode} ${baselinebc}

function run_passes() {
    for pass in "$@"; do
        opt --load-pass-plugin=${L}/lib/Alaska.so --passes=${pass} ${bitcode} -o ${bitcode}
    done
}

# First round of passes
run_passes alaska-prepare alaska-replace alaska-translate
# link the translate runtime
llvm-link ${bitcode} --internalize ${B}/translate.bc -o ${bitcode}
# Second round of passes
run_passes alaska-escape alaska-lower alaska-inline

llvm-dis ${bitcode} -o ${B}/final.ll

mkdir -p ${B}/dist

# now we have the final bitcode. use llc to generate a riscv object file
llc -O3 -mtriple=riscv64-unknown-linux-gnu -filetype=obj ${bitcode} -o ${B}/dist/final.o


llvm-link ${baselinebc} --internalize ${B}/translate.bc -o ${baselinebc}
llc -O3 -mtriple=riscv64-unknown-linux-gnu -filetype=obj ${baselinebc} -o ${B}/dist/baseline.o



LDS=${ROOT}/compiler/ldscripts/alaska-riscv64.ld

cp ${B}/riscv/runtime/libalaska.so.2 ${B}/dist/
cp ${LDS} ${B}/dist/

ls -la ${B}/dist

pushd ${B}/dist

FINAL_ARGS="-lm -lpthread -L. "
FINAL_ARGS+="-Wl,-z,now -l:libalaska.so.2 -Wl,-rpath=\$ORIGIN "
# FINAL_ARGS+="-Wl,--whole-archive ${B}/dist/libalaska.a "
FINAL_ARGS+="-T./alaska-riscv64.ld "

# now link the final executable with /opt/riscv tools
/opt/riscv/bin/riscv64-unknown-linux-gnu-gcc ./final.o -o ./alaska ${FINAL_ARGS}
/opt/riscv/bin/riscv64-unknown-linux-gnu-gcc ./baseline.o -o ./baseline ${FINAL_ARGS}

echo "gcc ./final.o -o ./alaska ${FINAL_ARGS}" > ${B}/dist/build.sh
popd
