#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

for argument; do
	remote_command=$argument
done
case "$remote_command" in
'cat /sys/class/misc/ums9117-nand-raw/device/geometry 3</dev/ums9117-nand-raw')
	printf '%s' "${FPLINUX_GEOMETRY:?}"
	exit 0
	;;
esac
if [ -n "${FPLINUX_REMOTE_COMMANDS:-}" ]; then
	printf '%s\n' "$remote_command" >>"$FPLINUX_REMOTE_COMMANDS"
fi
case "${FPLINUX_STREAM_MODE:?}" in
success)
	printf 'raw-page-bytes'
	printf 'remote diagnostic' >&2
	;;
short)
	printf 'short'
	;;
nonzero)
	printf 'partial'
	printf 'NAND read failed' >&2
	exit 8
	;;
timeout)
	printf '%s' "$$" >"${FPLINUX_STREAM_PID:?}"
	exec sleep 60
	;;
*) exit 64 ;;
esac
