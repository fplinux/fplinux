#!/bin/sh
set -eu

llvm_source=${1:?LLVM 23.1.6 source directory}
sysroot=${2:?Alpine 3.24 i586 sysroot}
runtime_build=${3:?Empty build directory}
runtime_install=${4:?Empty staging directory}
ninja_path=${5:?Absolute path to Ninja}
target=i586-alpine-linux-musl

cmake -S "$llvm_source/runtimes" -B "$runtime_build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_MAKE_PROGRAM="$ninja_path" \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_SYSTEM_PROCESSOR=i586 \
	-DCMAKE_C_COMPILER=clang -DCMAKE_C_COMPILER_TARGET="$target" \
	-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_COMPILER_TARGET="$target" \
	-DCMAKE_ASM_COMPILER=clang -DCMAKE_ASM_COMPILER_TARGET="$target" \
	-DCMAKE_SYSROOT="$sysroot" \
	-DCMAKE_C_FLAGS='-march=pentium-m -mfpmath=sse' \
	-DCMAKE_CXX_FLAGS='-march=pentium-m -mfpmath=sse' \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DLLVM_ENABLE_RUNTIMES='compiler-rt;libunwind' \
	-DLLVM_INCLUDE_TESTS=OFF \
	-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
	-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
	-DCOMPILER_RT_INSTALL_PATH="/usr/lib/llvm23/lib/clang/23" \
	-DCOMPILER_RT_BUILD_SANITIZERS=OFF \
	-DCOMPILER_RT_BUILD_XRAY=OFF \
	-DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
	-DCOMPILER_RT_BUILD_PROFILE=OFF \
	-DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
	-DCOMPILER_RT_BUILD_MEMPROF=OFF \
	-DCOMPILER_RT_BUILD_ORC=OFF \
	-DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
	-DLIBUNWIND_ENABLE_SHARED=OFF \
	-DLIBUNWIND_HAS_NODEFAULTLIBS_FLAG=OFF

cmake --build "$runtime_build" --parallel 2 --target \
	clang_rt.builtins-i386 clang_rt.crtbegin-i386 clang_rt.crtend-i386 unwind_static
for component in clang_rt.builtins-i386 clang_rt.crtbegin-i386 clang_rt.crtend-i386 unwind; do
	DESTDIR="$runtime_install" cmake --install "$runtime_build" --component "$component"
done

runtime_lib="$runtime_install/usr/lib/llvm23/lib/clang/23/lib"
mv -- "$runtime_lib/linux" "$runtime_lib/$target"
ln -s -- "$target" "$runtime_lib/linux"
