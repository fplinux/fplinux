#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture the ARMADA scene and its before-and-after rotation example.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 OUTPUT-DIRECTORY" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../../.." && pwd)
showcase=$root/alpine/aports/fplinux-showcase
rotate=$root/alpine/aports/fplinux-rotate
output=$1
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"

png() {
	python3 "$root/tests/host_tool/gui/support/rgb565_png.py" "$1" "$2" "$3" "$output/$4"
}

# The pinned libdrm pkg-config output is a list of compiler arguments.
# shellcheck disable=SC2046
ui_host_compile -O2 -I"$root/include/fplinux" -I"$showcase" -I"$screens_dir" \
	-isystem /opt/quality/include $(pkg-config --cflags libdrm) \
	"$screens_dir/armada-screen.c" "$screens_dir/screen-frame.c" \
	"$showcase/armada-scene.c" "$showcase/armada-renderer.c" \
	"$showcase/armada-geometry.c" \
	"$showcase/armada-gpu-renderer.c" "$root/lib/fplinux-shared/fplinux-gpu.c" \
	"$root/lib/fplinux-shared/fplinux-gpu-frame.c" \
	"$showcase/armada-storyboard.c" \
	$(pkg-config --libs libdrm) -ldl -o "$work/armada-screen"
ui_host_compile -O2 -I"$rotate" -I"$screens_dir" \
	"$screens_dir/rotate-screen.c" "$screens_dir/screen-frame.c" \
	"$rotate/fplinux-rotate-core.c" -o "$work/rotate-screen"

mkdir -p -- "$output"
# 6 s and 22 s into the 38 s ARMADA loop, before the closing metrics card.
armada_frame=180
rotate_frame=660
"$work/armada-screen" "$armada_frame" "$work/armada.rgb565"
png 240 320 "$work/armada.rgb565" armada.png
# Use one portrait scene for both sides of the rotation example.
"$work/armada-screen" "$rotate_frame" "$work/portrait.rgb565"
"$work/rotate-screen" "$work/portrait.rgb565" 240 320 270 "$work/rotate-source.rgb565"
"$work/rotate-screen" "$work/rotate-source.rgb565" 320 240 90 "$work/rotate-result.rgb565"
png 320 240 "$work/rotate-source.rgb565" rotate-source.png
png 240 320 "$work/rotate-result.rgb565" rotate-result.png
