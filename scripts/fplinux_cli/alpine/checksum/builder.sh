#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

export SRCDEST=$1 SOURCE_DATE_EPOCH=$2
abuild checksum
exec apkbuild-lint APKBUILD
