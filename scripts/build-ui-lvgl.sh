#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the pinned host LVGL library used by UI tests and documentation captures.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 BUILD-DIRECTORY" >&2
	exit 2
fi

scripts_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$scripts_dir/.." && pwd)
directory=$1
aport=$root/alpine/aports/fplinux-lvgl
mkdir -p -- "$directory/source"
tar -xzf /opt/quality/src/lvgl.tar.gz --strip-components=1 -C "$directory/source"
install -Dm644 "$directory/source/src/libs/lodepng/lodepng.h" \
	"$directory/source/include/lvgl/src/libs/lodepng/lodepng.h"
for patch in shared-abi canvas-cache-drop; do
	patch -s -p1 -d "$directory/source" -i "$aport/$patch.patch"
done
cmake -S "$directory/source" -B "$directory/build" -G Ninja \
	-DCMAKE_C_COMPILER=cc -DBUILD_SHARED_LIBS=OFF \
	-DLV_BUILD_CONF_PATH="$aport/lv_conf.h" \
	-DLV_BUILD_SET_CONFIG_OPTS=ON -DLV_BUILD_LVGL_H_SYSTEM_INCLUDE=ON \
	-DLV_BUILD_INSTALL=OFF -DLV_FETCH_DEPENDENCIES=OFF \
	-DLV_USE_FIND_PACKAGE=OFF -DLV_USE_PKG_CONFIG=OFF
# Bound compiler fan-out for the host test runtime.
cmake --build "$directory/build" --target lvgl --parallel 2
