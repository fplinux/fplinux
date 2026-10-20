#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

export SRCDEST=$1 SOURCE_DATE_EPOCH=$2
if [ "$3" = offline ]; then
	export ABUILD_USERCONF="${0%/*}/offline.sh"
	abuild fplinux_checksum_sources checksum
else
	abuild checksum
fi
exec apkbuild-lint APKBUILD
