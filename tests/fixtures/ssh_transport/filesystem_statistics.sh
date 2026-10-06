#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

df() {
	printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\n'
	printf 'df: cannot find mount point\n' >&2
	return 1
}

stat() {
	printf '%s\n' "${FPLINUX_AVAILABLE_BLOCKS:?} 4096"
}
