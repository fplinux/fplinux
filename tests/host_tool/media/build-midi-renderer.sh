#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 BUILD-DIRECTORY" >&2
	exit 2
fi

directory=$1
source_root=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p -- "$directory/include" "$directory/bank"
sed 's/\r$//' /opt/quality/src/tsf.h >"$directory/include/tsf.h"
patch -d "$directory/include" -p1 --input="$source_root/alpine/aports/fplinux-midi/borrowed-s16.patch"
tar --no-same-owner -xzf /opt/quality/src/soundfont.tar.gz --strip-components=1 -C "$directory/bank"
