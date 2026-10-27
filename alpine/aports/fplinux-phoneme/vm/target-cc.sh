#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Target toolchain for the CLDC-HI build, installed as gcc, g++, as, ar, nm,
# ranlib and strip in GNU_TOOLS_DIR/bin: clang for ARMv7 hard-float musl with
# the FPLinux sysroot. All VM code is ARM, not Thumb: the interpreter enters
# natives with "mov pc". The upstream linux_arm configuration targets a
# soft-float GCC, so its -m32 and soft-float flags are dropped, and the
# generated interpreter is converted from divided to unified assembler syntax
# for the LLVM assembler (FPLINUX_PM_PRE_UAL_SED).
set -eu

sysroot=${CBUILDROOT:?}
pre_ual=${FPLINUX_PM_PRE_UAL_SED:?}
target=armv7-alpine-linux-musleabihf
llvm=/usr/lib/llvm22/bin
tool=${0##*/}

case ${tool} in
ar | nm | ranlib | strip) exec "${llvm}/llvm-${tool}" "$@" ;;
as | gcc | g++) ;;
*)
	echo "target-cc.sh: unknown tool name ${tool}" >&2
	exit 2
	;;
esac

for argument; do
	shift
	case ${argument} in
	-m32 | --32 | -mno-fpu | -msoft-float | -mfloat-abi=softfp | -mfpu=vfp | \
		-mcpu=strongarm | -mapcs-frame | -fno-optional-diags)
		continue
		;;
	*) ;;
	esac
	set -- "$@" "${argument}"
done

set -- --target="${target}" -mcpu=cortex-a7 -mfpu=vfpv4 -mfloat-abi=hard \
	-marm "$@"

if [ "${tool}" = as ]; then
	converted=$(mktemp -d)
	for argument; do
		shift
		case ${argument} in
		*.s)
			unified=${converted}/${argument##*/}
			sed -f "${pre_ual}" "${argument}" >"${unified}"
			argument=${unified}
			;;
		*) ;;
		esac
		set -- "$@" "${argument}"
	done
	status=0
	clang -c -x assembler "$@" || status=$?
	rm -rf "${converted:?}"
	exit "${status}"
fi

links=true
for argument; do
	case ${argument} in
	-c | -S | -E | -M | -MM) links=false ;;
	*) ;;
	esac
done

# The VM uses no C++ library facilities (no exceptions, RTTI or STL), and
# the FPLinux sysroot has no static libstdc++.
driver=clang
if [ "${tool}" = g++ ]; then
	driver=clang++
	set -- -std=gnu++98 -Wno-register -Wno-c++11-narrowing -nostdlib++ "$@"
fi
if ${links}; then
	set -- -fuse-ld=lld -no-pie "$@"
else
	set -- -fno-pie "$@"
fi
exec "${driver}" --sysroot="${sysroot}" "$@"
