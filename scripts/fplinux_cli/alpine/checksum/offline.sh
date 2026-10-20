#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

# Loaded by abuild before it evaluates APKBUILD and dispatches these commands.
fplinux_checksum_sources() {
	# abuild supplies source, SRCDEST and pkgname when it dispatches this function.
	# shellcheck disable=SC2154
	for checksum_source in $source; do
		is_remote "$checksum_source" || continue
		checksum_file=$SRCDEST/$(filename_from_uri "$checksum_source")
		if [ ! -f "$checksum_file" ]; then
			error "offline checksum requires cached source: ${checksum_file##*/}; run ./fplinux checksum $pkgname without --offline first"
			return 1
		fi
	done
}
