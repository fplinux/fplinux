#!/bin/sh
set -eu

headers_source=${1:?SPIRV-Headers source directory}
tools_source=${2:?SPIRV-Tools source directory}
translator_source=${3:?SPIRV-LLVM-Translator source directory}
build=${4:?Empty SPIR-V build directory}
llvm_build=${5:?Completed LLVM 23.1.6 build directory}
stage=${6:?LLVM staging directory}
ninja_path=${7:?Absolute path to Ninja}

cmake -S "$headers_source" -B "$build/headers" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$ninja_path" -DCMAKE_INSTALL_PREFIX=/usr
DESTDIR="$stage" cmake --install "$build/headers"

cmake -S "$tools_source" -B "$build/tools" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$ninja_path" -DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	-DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG' \
	-DPython3_EXECUTABLE=/usr/bin/python3.14 \
	-DSPIRV-Headers_SOURCE_DIR="$headers_source" -DSPIRV_SKIP_TESTS=ON \
	-DSPIRV_WERROR=OFF
cmake --build "$build/tools" --parallel 2
DESTDIR="$stage" cmake --install "$build/tools" --strip

# Select the staged Khronos package and the complete LLVM build-tree exports.
PKG_CONFIG_PATH='' PKG_CONFIG_LIBDIR="$stage/usr/lib/pkgconfig:$stage/usr/share/pkgconfig" \
	PKG_CONFIG_SYSROOT_DIR="$stage" \
	cmake -S "$translator_source" -B "$build/translator" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$ninja_path" -DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_RPATH=/usr/lib/llvm23/lib \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	-DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG' \
	-DCCACHE_ALLOWED=OFF -DLLVM_DIR="$llvm_build/lib/cmake/llvm" \
	-DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$headers_source" \
	-DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=ON
cmake --build "$build/translator" --parallel 2
DESTDIR="$stage" cmake --install "$build/translator" --strip
