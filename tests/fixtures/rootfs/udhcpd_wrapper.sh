#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

exec "$(dirname "$0")/../../bin/busybox" udhcpd "$@"
