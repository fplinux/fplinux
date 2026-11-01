#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture the actual Clock application in the pinned project environment.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
	echo "usage: $0 OUTPUT-DIRECTORY [PREPARED-INPUTS-DIRECTORY [NATIVE-BUILD-DIRECTORY]]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
output=$1
native=${3-}
work=$(mktemp -d)
# shellcheck source=site/scripts/screens/screen-captures.sh
. "$screens_dir/screen-captures.sh"
cleanup() {
	screen_captures_stop
	rm -rf -- "${work:?}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Capture one task: WIDTH HEIGHT [OUTPUT-SUFFIX].
capture() {
	read -r width height suffix <<EOF
$1
EOF
	frames=$work/$width"x"$height
	mkdir -- "$frames"
	"$native/clocks-screens" "$width" "$height" \
		"$native/clocks" "$screens_dir/clocks-keyboard.xkb" \
		"$frames.sock" "$frames"
	read -r frame_width frame_height <"$frames/frame-size"
	for frame in "$frames"/*.rgb565; do
		state=${frame##*/}
		state=${state%.rgb565}
		python3 "$screens_dir/rgb565-png.py" "$frame_width" "$frame_height" \
			"$frame" "$output/$state$suffix.png"
	done
}

mkdir -p -- "$output"
if [ -z "$native" ]; then
	native=$work/build
	sh "$screens_dir/build-clocks.sh" "$native" "${2-}"
fi
cat >"$work/captures" <<EOF
128 160 -128x160
240 320
320 240 -320x240
EOF
screen_captures_run capture "$work/captures" "$work/capture-logs"
