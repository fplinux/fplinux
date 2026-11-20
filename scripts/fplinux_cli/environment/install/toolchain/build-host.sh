#!/bin/sh
set -eu

llvm_build=${1:?Configured LLVM 23.1.6 build directory}
stage=${2:?Empty installation directory}

cmake --build "$llvm_build" --parallel 2 --target llvm-tblgen clang-tblgen
cmake --build "$llvm_build" --parallel 2 --target LLVM
cmake --build "$llvm_build" --parallel 2 --target clang-cpp
cmake --build "$llvm_build" --parallel 2
DESTDIR="$stage" cmake --install "$llvm_build" --strip
