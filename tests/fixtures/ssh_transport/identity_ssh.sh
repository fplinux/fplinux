#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

printf '%s\n' "$@" >"${FPLINUX_SSH_ARGUMENTS:?}"
printf '%s\n' "${FPLINUX_SESSION_ID:?}"
