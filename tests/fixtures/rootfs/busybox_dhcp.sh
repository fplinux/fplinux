#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

[ "$1" = udhcpd ] || exit 9
shift
[ "$#" = 2 ] && [ "$1" = -f ] || exit 10
[ "$2" = /run/fplinux-udhcpd.conf ] || exit 11
printf 'foreground DHCP started\n'
