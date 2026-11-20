#!/bin/sh
set -eu

stage=${1:?Complete native toolchain staging directory}
key=${2:?Private local APK signing key path}
package=${3:?APK output file}

# APK records the archive size in its installed database. RSA signature bytes
# change under a new key, so keep the fixed-size signature block uncompressed.
apk --no-network mkpkg --compression none --sign-key "$key" -F "$stage" -o "$package" \
	-I name:fplinux-build-toolchain -I version:23.1.6-r0 -I arch:x86_64 \
	-I 'description:Native LLVM, Clang, LLD and SPIR-V build tools for FPLinux' \
	-I 'license:Apache-2.0 WITH LLVM-exception AND Apache-2.0 AND MIT AND CC-BY-4.0 AND NCSA' \
	-I 'depends:python3~3.14 perl pkgconf musl-dev libgcc-static libstdc++-dev fortify-headers>=1.1-r2 libffi-dev zlib-dev zstd-dev so:libc.musl-x86_64.so.1 so:libgcc_s.so.1 so:libstdc++.so.6 so:libcurl.so.4 so:libffi.so.8 so:libxml2.so.2 so:libz.so.1 so:libzstd.so.1' \
	-I 'provides:clang23=23.1.6-r0 clang23-dev=23.1.6-r0 clang23-analyzer=23.1.6-r0 llvm23=23.1.6-r0 llvm23-dev=23.1.6-r0 lld23=23.1.6-r0 spirv-headers=1.4.357.0-r0 spirv-tools=1.4.357.0-r0 spirv-tools-dev=1.4.357.0-r0 spirv-llvm-translator=23.1.1-r0 spirv-llvm-translator-dev=23.1.1-r0'
