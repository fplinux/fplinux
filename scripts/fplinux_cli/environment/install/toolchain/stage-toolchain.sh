#!/bin/sh
set -eu

stage=${1:?LLVM and SPIR-V staged installation}
i586_install=${2:?Staged i586 runtime installation}
scudo_library=${3:?Built x86_64 Scudo shared library}
patchelf=${4:?Absolute path to patchelf}
llvm_source=${5:?LLVM source directory}
headers_source=${6:?SPIRV-Headers source directory}
tools_source=${7:?SPIRV-Tools source directory}
translator_source=${8:?SPIRV-LLVM-Translator source directory}

llvm_prefix="$stage/usr/lib/llvm23"
for tool in clang clang++ clang-format lld ld.lld llvm-config scan-build; do
	test -x "$llvm_prefix/bin/$tool"
done
test -f "$llvm_prefix/lib/libLLVM.so"
test -f "$llvm_prefix/lib/libclang-cpp.so"
test -f "$llvm_prefix/lib/libclang.so"
test -f "$llvm_prefix/share/clang/clang-format-diff.py"
test -f "$stage/usr/lib/pkgconfig/SPIRV-Tools.pc"
test -f "$stage/usr/lib/pkgconfig/LLVMSPIRVLib.pc"

mkdir -p "$stage/usr/bin" "$stage/usr/lib/python3.14/site-packages"
for tool in clang clang++ clang-format lld ld.lld llvm-config scan-build; do
	ln -s "../lib/llvm23/bin/$tool" "$stage/usr/bin/$tool"
done
ln -s llvm23/lib/libclang.so "$stage/usr/lib/libclang.so"
mv -- "$llvm_prefix/lib/python3.14/site-packages/clang" "$stage/usr/lib/python3.14/site-packages/"
mkdir -p "$stage/etc/clang23"
printf '%s\n' '-fstack-clash-protection' >"$stage/etc/clang23/x86_64-alpine-linux-musl.cfg"

i586_lib=usr/lib/llvm23/lib/clang/23/lib/i586-alpine-linux-musl
for file in libclang_rt.builtins-i386.a clang_rt.crtbegin-i386.o clang_rt.crtend-i386.o; do
	install -Dm644 "$i586_install/$i586_lib/$file" "$stage/$i586_lib/$file"
done
install -Dm644 "$i586_install/usr/lib/libunwind.a" "$llvm_prefix/lib/fplinux/i586/libunwind.a"

install -Dm755 "$scudo_library" "$stage/usr/lib/libscudo.so"
"$patchelf" --set-soname libscudo.so "$stage/usr/lib/libscudo.so"
"$patchelf" --add-needed libscudo.so "$llvm_prefix/bin/lld"

licenses="$stage/usr/share/licenses/fplinux-build-toolchain"
install -Dm644 "$llvm_source/LICENSE.TXT" "$licenses/LLVM.txt"
install -Dm644 "$headers_source/LICENSE" "$licenses/SPIRV-Headers.txt"
install -Dm644 "$headers_source/LICENSES/MIT.txt" "$licenses/MIT.txt"
install -Dm644 "$headers_source/LICENSES/CC-BY-4.0.txt" "$licenses/CC-BY-4.0.txt"
install -Dm644 "$tools_source/LICENSE" "$licenses/SPIRV-Tools.txt"
install -Dm644 "$translator_source/LICENSE.TXT" "$licenses/SPIRV-LLVM-Translator.txt"
