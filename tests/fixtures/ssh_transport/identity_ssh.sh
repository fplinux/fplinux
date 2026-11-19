#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

printf '%s\n' "$@" >"${FPLINUX_SSH_ARGUMENTS:?}"
command=
for argument; do
	command=$argument
done
case "$command" in
fplinux-session-id) printf '%s\n' "${FPLINUX_SESSION_ID:?}" ;;
'uname -r && '*)
	printf '%s\n' "${FPLINUX_KERNEL_RELEASE:?}"
	printf '%064d\n' 0 | tr 0 9
	;;
*) exit 9 ;;
esac
