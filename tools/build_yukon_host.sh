

unset LD_LIBRARY_PATH

ROOT=$RISCV

export CC=/usr/bin/gcc
export CXX=/usr/bin/g++
export AS=/usr/bin/as
export LD=/usr/bin/ld

make clean

REV="$(whoami)-$(git rev-parse --short HEAD)"
if [[ $(git diff --stat) != '' ]]; then
  REV="${REV}-dirty"
fi


cmake ../ \
      -DALASKA_REVISION="${REV}" \
      -DALASKA_ENABLE_COMPILER=OFF \
      -DALASKA_ENABLE_TESTING=OFF \
      -DALASKA_ENABLE_LOGGING=OFF \
      -DALASKA_CORE_ONLY=ON \
      -DALASKA_YUKON=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DALASKA_SIZE_BITS=32

make -j
