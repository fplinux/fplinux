#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
fplinux_archive=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 1
PATH="$fplinux_archive/tools/linux:$PATH"
export PATH
case "${1-}" in
run | console | verify | prepare | nand) ;;
*) set -- run "$@" ;;
esac
exec "$fplinux_archive/runtime/lib/ld-musl-x86_64.so.1" \
	--library-path "$fplinux_archive/runtime/lib" \
	"$fplinux_archive/runtime/bin/python3.15" -B -I "$fplinux_archive/fplinux.py" "$@"
