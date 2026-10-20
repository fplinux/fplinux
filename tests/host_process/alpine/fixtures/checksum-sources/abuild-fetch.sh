#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

[ "$1" = -d ]
checksum_cache=$2
checksum_uri=$3
checksum_marker=${FPLINUX_CHECKSUM_FETCH_MARKER:?}
printf '%s\n' "$checksum_uri" >>"$checksum_marker"
checksum_filename=${checksum_uri%%::*}
checksum_filename=${checksum_filename##*/}
if [ ! -f "$checksum_cache/$checksum_filename" ]; then
	printf '%s\n' 'test downloader refused an uncached source' >&2
	exit 99
fi
exec /usr/bin/abuild-fetch "$@"
