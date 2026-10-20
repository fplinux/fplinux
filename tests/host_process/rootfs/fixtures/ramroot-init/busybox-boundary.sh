#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# Keep the real shell and copy operations; only the kernel mount and root-init
# handoff are replaced so the script can run in an isolated host filesystem.
case "$1" in
mount)
	exit 0
	;;
switch_root)
	[ "$2" = -c ]
	[ "$3" = /dev/console ]
	shift 3
	exec /libexec/busybox chroot "$@"
	;;
*)
	exec /libexec/busybox "$@"
	;;
esac
