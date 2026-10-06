#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

count=0
[ ! -f "${FPLINUX_KEYSCAN_COUNT:?}" ] || count=$(cat "$FPLINUX_KEYSCAN_COUNT")
count=$((count + 1))
printf '%s\n' "$count" >"$FPLINUX_KEYSCAN_COUNT"
[ "$count" -gt 1 ] || exit 1
printf '%s\n' "10.23.45.2 ssh-ed25519 ${FPLINUX_HOST_KEY:?}"
