#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

llvm_archive=$1
headers_archive=$2
spirv_tools_archive=$3
translator_archive=$4
i586_musl=$5
i586_libstdcpp=$6
shift 6

apk add --no-network --no-cache "$@"

recipes=/recipe/scripts/fplinux_cli/environment/install/toolchain
llvm=/work/source/llvm
headers=/work/source/spirv-headers
spirv_tools=/work/source/spirv-tools
translator=/work/source/spirv-llvm-translator
ninja=/usr/lib/ninja-build/bin/ninja
export HOME=/work/home
mkdir -p "$HOME" /work/out /work/i586-sysroot "$llvm" "$headers" "$spirv_tools" "$translator"

unpack_source() {
	tar --extract --no-same-owner --file "$1" --strip-components=1 --directory "$2"
}

unpack_source "$llvm_archive" "$llvm"
unpack_source "$headers_archive" "$headers"
unpack_source "$spirv_tools_archive" "$spirv_tools"
unpack_source "$translator_archive" "$translator"
tar --extract --no-same-owner --file "$i586_musl" --exclude='.*' --directory /work/i586-sysroot
tar --extract --no-same-owner --file "$i586_libstdcpp" --exclude='.*' --directory /work/i586-sysroot

patch --fuzz=0 -d "$llvm" -p1 -i "$recipes/llvm23-alpine-driver.patch"
sh "$recipes/configure-host.sh" "$llvm" /work/llvm-build "$ninja"
sh "$recipes/build-host.sh" /work/llvm-build /work/stage
sh "$recipes/build-spirv.sh" "$headers" "$spirv_tools" "$translator" \
	/work/spirv-build /work/llvm-build /work/stage "$ninja"
sh "$recipes/build-i586-runtime.sh" "$llvm" /work/i586-sysroot \
	/work/i586-build /work/i586-install "$ninja"
sh "$recipes/build-host-scudo.sh" "$llvm" /work/scudo-build "$ninja"
sh "$recipes/stage-toolchain.sh" /work/stage /work/i586-install \
	/work/scudo-build/compiler-rt/lib/linux/libclang_rt.scudo_standalone-x86_64.so \
	/usr/bin/patchelf "$llvm" "$headers" "$spirv_tools" "$translator"

# This key authenticates the locally generated build package inside the environment.
private_key=/work/fplinux-toolchain.rsa
(
	umask 077
	openssl genrsa -out "$private_key" 2048
)
openssl rsa -in "$private_key" -pubout -out /work/out/fplinux-toolchain.rsa.pub
sh "$recipes/package-toolchain.sh" /work/stage "$private_key" /work/out/toolchain.apk
unlink "$private_key"
