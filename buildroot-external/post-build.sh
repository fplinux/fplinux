#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Stamp the image with the digest of the sources it was built from.  Without
# it nothing inside a running phone says which build it is: the kernel version
# is fixed, the build timestamp is pinned for reproducibility, and the module
# vermagic is identical across rebuilds, so a module from a foreign tree loads
# without a word.

set -eu

target="$1"
rm -f "$target/etc/fplinux-build"
printf '%s\n' "${FPLINUX_WORKSPACE_DIGEST:-unknown}" >"$target/etc/fplinux-build"
chmod 0444 "$target/etc/fplinux-build"
