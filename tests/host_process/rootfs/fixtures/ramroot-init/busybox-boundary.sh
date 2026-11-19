#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# Keep the real shell and copy operations; only the kernel mount and root-init
# handoff are replaced so the script can run in an isolated host filesystem.
case "$1" in
mount)
	if [ "$2" = -t ] && [ "$3" = squashfs ] && [ "$6" = /board.squashfs ]; then
		printf '%s\n' "$5" "$7" >/run/board-mount
		[ ! -f /fail-board-mount ] || exit 1
	elif [ "$2" = -t ] && [ "$3" = overlay ]; then
		printf '%s\n' "$6" >/run/overlay-options
	fi
	exit 0
	;;
sleep)
	# Terminate the isolated shell when it enters its permanent boot failure path.
	kill -TERM "$PPID"
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
