#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# Keep the production scripts and filesystem operations. Replace only automatic
# ConfigFS directories, network ioctls and the Dropbear process boundary.
case "${0##*/}" in
mkdir)
	/libexec/busybox mkdir "$@"
	case "$1" in
	/sys/kernel/config/usb_gadget/fplinux)
		/libexec/busybox mkdir "$1/configs" "$1/functions" "$1/os_desc" "$1/strings"
		;;
	*/configs/c.1) /libexec/busybox mkdir "$1/strings" ;;
	*/functions/rndis.usb0) /libexec/busybox mkdir -p "$1/os_desc/interface.rndis" ;;
	esac
	;;
ln) exec /libexec/busybox ln "$@" ;;
ifconfig) printf '%s\n' "$*" >>/run/ifconfig ;;
ip | route) exit 0 ;;
busybox)
	if [ "$1" = timeout ]; then
		# Simulate the RAM host-key preparation deadline without waiting.
		[ ! -e /run/random-pending ]
		exit
	fi
	exec /libexec/busybox "$@"
	;;
dropbearkey)
	[ "$1" = -t ] && [ "$2" = ed25519 ] && [ "$3" = -f ]
	printf 'generated host key\n' >"$4"
	;;
dropbear) printf '%s\n' "$@" >/run/dropbear ;;
*) exit 2 ;;
esac
