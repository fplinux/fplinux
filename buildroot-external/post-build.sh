#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Stamp the root filesystem with its exact Buildroot recipe.  The kernel carries
# an independent content-derived device identity in CONFIG_LOCALVERSION, so a
# kernel-only change does not force an otherwise identical rootfs rebuild.

set -eu

target="$1"
pak=$(find "$target" -type f -iname '*.pak' -print -quit)
if [ -n "$pak" ]; then
	printf 'fplinux: Quake game data must not enter the root filesystem: %s\n' \
		"$pak" >&2
	exit 1
fi

: "${FPLINUX_BUILDROOT_RECIPE:?missing Buildroot causal recipe}"
rm -f "$target/etc/fplinux-build"
printf 'buildroot=%s\n' "$FPLINUX_BUILDROOT_RECIPE" >"$target/etc/fplinux-build"
chmod 0444 "$target/etc/fplinux-build"
