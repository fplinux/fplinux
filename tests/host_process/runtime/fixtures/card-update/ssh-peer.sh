#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu
[ "$1" = -T ]
scenario=$2
work=$3
printf '%s\n' "$4" >"$work/command"
case "$scenario" in
transfer)
	mkdir "$work/received"
	cd "$work/received"
	tar -xf -
	sha256sum -c ../transfer.sha256 >/dev/null
	printf 'updated 222222222222\n'
	;;
already-active)
	printf 'active 222222222222\n'
	;;
disconnect)
	head -c 4096 >/dev/null
	printf 'connection lost during input\n' >&2
	exit 255
	;;
*) exit 1 ;;
esac
