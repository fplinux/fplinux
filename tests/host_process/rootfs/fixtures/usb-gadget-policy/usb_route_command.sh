#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

# Replace the external route query; filters deliberately do not affect output.
set -eu

if [ "${FPLINUX_TEST_ROUTE_STATUS:?}" -ne 0 ]; then
	printf '%s\n' 'routing query failed' >&2
	exit "${FPLINUX_TEST_ROUTE_STATUS:?}"
fi
cat "${FPLINUX_TEST_ROUTE_TABLE:?}"
