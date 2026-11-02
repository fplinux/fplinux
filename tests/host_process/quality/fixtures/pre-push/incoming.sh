#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

printf '%s\n' 'incoming code must not execute' >"${FPLINUX_PRE_PUSH_TEST_MARKER:?}"
exit 89
