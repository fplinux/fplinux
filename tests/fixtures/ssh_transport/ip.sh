#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

case "$*" in
"-4 -j address show dev usb0")
	printf '%s\n' "${FPLINUX_IP_ADDRESS:?}"
	;;
"-4 -j route show 10.23.45.0/30")
	printf '%s\n' "${FPLINUX_IP_ROUTE:?}"
	;;
*) exit 2 ;;
esac
