#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# The sourced recipe selects its own source directory and build environment.
# shellcheck disable=SC2034
srcdir=$2
# shellcheck source=/dev/null
. "$1"
# Configure and make are external peers; no Bash binary is compiled here.
make() {
	:
}
# shellcheck disable=SC2154
mkdir -p "$builddir"
cp "$3" "$builddir/configure"
chmod +x "$builddir/configure"
cd "$builddir"
build
