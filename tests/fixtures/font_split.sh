#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

srcdir=$3
pkgdir=$4
subpkgdir=$5
depends=unsplit
provides=unsplit
export srcdir subpkgname
# APKBUILD is the subject; the fixture replaces only Alpine's move helper.
# shellcheck source=/dev/null
. "$1"

amove() {
	for font_path; do
		mkdir -p "$subpkgdir/${font_path%/*}"
		mv "$pkgdir/$font_path" "$subpkgdir/$font_path"
	done
}

cd "$pkgdir"
case "$2" in
6x12)
	subpkgname=fplinux-font-terminus-6x12
	font_6x12
	;;
8x16)
	subpkgname=fplinux-font-terminus-8x16
	font_8x16
	;;
*) exit 2 ;;
esac
printf 'depends=%s\nprovides=%s\n' "$depends" "$provides"
