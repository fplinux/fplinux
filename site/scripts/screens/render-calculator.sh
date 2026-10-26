#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture the actual Calculator application in the pinned project environment.
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 OUTPUT-DIRECTORY" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
output=$1
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

capture() {
	width=$1
	height=$2
	suffix=$3
	frames=$work/$width"x"$height
	mkdir -- "$frames"
	"$work/build/calculator-screens" "$width" "$height" \
		"$work/build/calculator" "$screens_dir/calculator-keyboard.xkb" \
		"$work/session.sock" "$frames"
	read -r frame_width frame_height <"$frames/frame-size"
	for state in calculator-pending calculator-result calculator-number calculator-error calculator-limit calculator-menu; do
		python3 "$screens_dir/rgb565-png.py" "$frame_width" "$frame_height" \
			"$frames/$state.rgb565" "$output/$state$suffix.png"
	done
}

mkdir -p -- "$output"
sh "$screens_dir/build-calculator.sh" "$work/build"
capture 128 160 -128x160
capture 240 320 ''
capture 320 240 -320x240
