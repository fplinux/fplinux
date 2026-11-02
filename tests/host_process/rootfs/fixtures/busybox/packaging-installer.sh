#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# This fake replaces the external installer; the package recipe remains real.
set -eu
[ "$#" -eq 2 ] && [ "$2" = --symlinks ] || exit 64
install -Dm755 busybox "$1/bin/busybox"
while IFS= read -r applet; do
	mkdir -p "$1$(dirname "$applet")"
	ln -s /bin/busybox "$1$applet"
done <busybox.links
