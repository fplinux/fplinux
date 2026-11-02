#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

printf 'resize2fs:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
exit "${FPLINUX_RESIZE2FS_STATUS:-0}"
