#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Render the documentation phone screens from the production UI sources.
#
# Terminal runs as a real application under a controlled Wayland peer. The
# other phone programs draw into RGB565 buffers through small host drivers.
# FPLINUX_SOURCES names the directory with the checksummed upstream archives
# (default: the project download cache).
set -eu

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
common=$root/tests/host_tool/gui/screens
aports=$root/alpine/aports
if [ -n "${FPLINUX_SOURCES:-}" ]; then
	sources=$FPLINUX_SOURCES
else
	data=$("$root/fplinux" data-path --check)
	sources=$data/downloads/alpine/sources
fi
output=$root/site/src/assets/screens
# 6 s and 22 s into the 38 s ARMADA loop, before the closing metrics card.
armada_frame=180
rotate_frame=660
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

compile() {
	cc -std=c11 -Wall -Wextra -Werror "$@"
}

png() {
	python3 "$root/tests/host_tool/gui/support/rgb565_png.py" "$1" "$2" "$3" "$output/$4"
}

aport_value() {
	sed -n "s/^$2=//p" "$aports/$1/APKBUILD"
}

# Extract an archive only when it matches the checksum its aport builds with.
extract_verified() {
	archive=$sources/$2
	expected=$(awk -v name="$2" '$2 == name { print $1 }' \
		"$aports/$1/APKBUILD")
	actual=$(sha512sum -- "$archive" | cut -d ' ' -f 1)
	if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
		echo "render.sh: $archive does not match $1/APKBUILD" >&2
		exit 1
	fi
	tar -xzf "$archive" -C "$work"
}

build_fonts() {
	version=$(aport_value fplinux-font-terminus pkgver)
	font=$work/terminus-font-$version
	extract_verified fplinux-font-terminus "terminus-font-$version.tar.gz"
	for size in 12 16; do
		python3 "$font/bin/bdftopsf.py" -2 -o "$work/ter-u${size}n.psf" \
			"$font/ter-u${size}n.bdf"
	done
}

render_armada_and_rotation() {
	showcase=$aports/fplinux-showcase
	rotate=$aports/fplinux-rotate
	# The pinned libdrm pkg-config output is a list of compiler arguments.
	# shellcheck disable=SC2046
	compile -O2 -I"$root/include/fplinux" -I"$showcase" -I"$screens_dir" -I"$common" \
		-isystem /opt/quality/include $(pkg-config --cflags libdrm) \
		"$screens_dir/armada-screen.c" "$common/screen-frame.c" \
		"$showcase/armada-scene.c" "$showcase/armada-renderer.c" \
		"$showcase/armada-geometry.c" \
		"$showcase/armada-gpu-renderer.c" "$root/lib/fplinux-shared/fplinux-gpu.c" \
		"$root/lib/fplinux-shared/fplinux-gpu-frame.c" \
		"$showcase/armada-storyboard.c" \
		$(pkg-config --libs libdrm) -ldl -o "$work/armada-screen"
	compile -O2 -I"$rotate" -I"$screens_dir" -I"$common" \
		"$screens_dir/rotate-screen.c" "$common/screen-frame.c" \
		"$rotate/fplinux-rotate-core.c" -o "$work/rotate-screen"
	"$work/armada-screen" "$armada_frame" \
		"$work/armada.rgb565"
	png 240 320 "$work/armada.rgb565" armada.png
	# Use one portrait scene for both sides of the rotation example.
	"$work/armada-screen" "$rotate_frame" \
		"$work/portrait.rgb565"
	"$work/rotate-screen" "$work/portrait.rgb565" 240 320 270 \
		"$work/rotate-source.rgb565"
	"$work/rotate-screen" "$work/rotate-source.rgb565" 320 240 90 \
		"$work/rotate-result.rgb565"
	png 320 240 "$work/rotate-source.rgb565" rotate-source.png
	png 240 320 "$work/rotate-result.rgb565" rotate-result.png
}

mkdir -p -- "$output"
build_fonts
sh "$root/tests/host_tool/gui/terminal/native/render-terminal.sh" "$output"
render_armada_and_rotation
