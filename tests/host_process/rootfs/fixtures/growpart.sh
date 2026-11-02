#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

printf 'growpart:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
if [ "${FPLINUX_GROWPART_STATUS:?}" -ge 2 ]; then
	printf 'growpart failed\n' >&2
fi
exit "$FPLINUX_GROWPART_STATUS"
