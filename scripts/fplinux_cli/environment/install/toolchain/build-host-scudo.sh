#!/bin/sh
set -eu

llvm_source=${1:?LLVM 23.1.6 source directory}
scudo_build=${2:?Empty build directory}
ninja_path=${3:?Absolute path to Ninja}

cmake -S "$llvm_source/runtimes" -B "$scudo_build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_MAKE_PROGRAM="$ninja_path" \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	-DCMAKE_CXX_FLAGS='-DSCUDO_DISABLE_TBI -DSCUDO_CAN_USE_PRIMARY64=0' \
	-DCMAKE_DISABLE_FIND_PACKAGE_LLVM=ON -DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON \
	-DLLVM_ENABLE_RUNTIMES=compiler-rt \
	-DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
	-DCOMPILER_RT_INCLUDE_TESTS=OFF \
	-DCOMPILER_RT_BUILD_BUILTINS=OFF -DCOMPILER_RT_BUILD_SANITIZERS=ON \
	-DCOMPILER_RT_BUILD_ORC=OFF \
	-DCOMPILER_RT_HAS_SCUDO_STANDALONE=ON \
	-DCOMPILER_RT_SANITIZERS_TO_BUILD=scudo_standalone \
	-DCOMPILER_RT_BUILD_GWP_ASAN=OFF
cmake --build "$scudo_build" --parallel 2 --target scudo_standalone
