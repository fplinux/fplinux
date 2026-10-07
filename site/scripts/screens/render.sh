#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Render the documentation phone screens from the production UI sources.
#
# The phone programs draw into RGB565 buffers; small host drivers call the same
# drawing code, save the frames and convert them to PNG without scaling.
# FPLINUX_SOURCES names the directory with the checksummed upstream archives
# (default: the project download cache).
set -eu

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
aports=$root/alpine/aports
sources=${FPLINUX_SOURCES:-$root/.cache/downloads/alpine/sources}
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
	python3 "$screens_dir/rgb565-png.py" "$1" "$2" "$3" "$output/$4"
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

build_libtsm() {
	version=$(aport_value fplinux-libtsm pkgver)
	tsm=$work/libtsm-$version
	extract_verified fplinux-libtsm "libtsm-$version.tar.gz"
	patch -s -d "$tsm" -p1 \
		<"$aports/fplinux-libtsm/0001-xterm-function-keys.patch"
	for unit in src/tsm/tsm-render.c src/tsm/tsm-screen.c \
		src/tsm/tsm-selection.c src/tsm/tsm-unicode.c \
		src/tsm/tsm-vte-charsets.c src/tsm/tsm-vte.c \
		src/shared/shl-htable.c external/wcwidth/wcwidth.c; do
		cc -c -O2 -std=gnu99 -D_GNU_SOURCE -I"$tsm/src/tsm" \
			-I"$tsm/src/shared" -I"$tsm/external" \
			-I"$tsm/external/wcwidth" "$tsm/$unit" \
			-o "$work/libtsm-$(basename "$unit" .c).o"
	done
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

# Arguments: width, height, PSF font chosen by fplinux-terminal for that width.
run_terminal() {
	mkdir -p -- "$work/$1x$2"
	"$work/terminal-screens" "$1" "$2" "$work/$3" \
		"$aports/fplinux-base/issue" "$aports/fplinux-base/hostname" \
		"$work/$1x$2"
}

render_terminal() {
	terminal=$aports/fplinux-terminal
	build_libtsm
	# The terminal needs only xkbcommon keysym values; libtsm ships them.
	compile -I"$root/include/fplinux" -I"$terminal" -I"$screens_dir" \
		-I"$tsm/src/tsm" -I"$tsm/external" \
		"$screens_dir/terminal-screens.c" "$screens_dir/screen-frame.c" \
		"$terminal/terminal-engine.c" "$terminal/terminal-input.c" \
		"$terminal/terminal-help.c" "$terminal/terminal-render.c" \
		"$root/lib/fplinux/fplinux-font.c" \
		"$root/lib/fplinux/fplinux-multitap.c" "$work"/libtsm-*.o \
		-o "$work/terminal-screens"
	# The shared default font is ter-u12n below 200 pixels, else ter-u16n.
	run_terminal 240 320 ter-u16n.psf
	for name in terminal-keypad terminal-menu terminal-modifiers; do
		png 240 320 "$work/240x320/$name.rgb565" "$name.png"
	done
	run_terminal 128 160 ter-u12n.psf
	png 128 160 "$work/128x160/terminal-keypad.rgb565" \
		terminal-keypad-128x160.png
}

render_brightness() {
	brightness=$aports/fplinux-brightness-ui
	compile -I"$root/include/fplinux" -I"$brightness" -I"$screens_dir" \
		"$screens_dir/brightness-screen.c" "$screens_dir/screen-frame.c" \
		"$brightness/brightness-ui.c" "$root/lib/fplinux/fplinux-font.c" \
		-o "$work/brightness-screen"
	"$work/brightness-screen" 7 "$work/ter-u16n.psf" \
		"$work/brightness.rgb565"
	png 240 320 "$work/brightness.rgb565" brightness.png
}

render_armada_and_rotation() {
	showcase=$aports/fplinux-showcase
	rotate=$aports/fplinux-rotate
	compile -O2 -I"$root/include/fplinux" -I"$showcase" -I"$screens_dir" \
		"$screens_dir/armada-screen.c" "$screens_dir/screen-frame.c" \
		"$showcase/armada-scene.c" "$showcase/armada-renderer.c" \
		"$showcase/armada-storyboard.c" "$root/lib/fplinux/fplinux-font.c" \
		-o "$work/armada-screen"
	compile -O2 -I"$rotate" -I"$screens_dir" \
		"$screens_dir/rotate-screen.c" "$screens_dir/screen-frame.c" \
		"$rotate/fplinux-rotate-core.c" -o "$work/rotate-screen"
	"$work/armada-screen" "$armada_frame" "$work/ter-u16n.psf" \
		"$work/armada.rgb565"
	png 240 320 "$work/armada.rgb565" armada.png
	# ARMADA renders only up to 240 pixels wide, so the 320x240 source is a
	# portrait frame turned by the same engine; turning it back is the result.
	"$work/armada-screen" "$rotate_frame" "$work/ter-u16n.psf" \
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
render_terminal
render_brightness
render_armada_and_rotation
