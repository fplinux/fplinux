#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

stage_root=$1
downloads=$2
package=$3
checksum_epoch=$4
source_cache=$downloads/alpine/sources
stage_aport=$stage_root/alpine/aports/$package

chmod 755 "$stage_root" "$stage_root/alpine" "$stage_root/alpine/aports"
install -d "$source_cache"
restore_ownership() {
	chown -R 0:0 "$stage_aport" "$source_cache"
}
trap restore_ownership EXIT HUP INT TERM
chown -R builder:builder "$stage_aport" "$source_cache"
su builder -s /bin/sh -- "$stage_root/checksum/builder.sh" "$source_cache" "$checksum_epoch"
