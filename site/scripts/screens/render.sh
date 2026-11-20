#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Update the documentation screens using the prepared Alpine quality tools.
set -eu

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
gui=$root/tests/host_tool/gui
output=$root/site/src/assets/screens
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sh "$gui/terminal/native/render-terminal.sh" "$work"
sh "$gui/screens/render-armada-rotate.sh" "$work"

mkdir -p -- "$output/terminal" "$output/armada" "$output/rotate"
cp -- "$work/terminal-keypad.png" "$work/terminal-menu.png" "$work/terminal-modifiers.png" \
	"$work/terminal-keypad-128x160.png" "$work/terminal-keypad-320x240.png" "$output/terminal/"
cp -- "$work/armada.png" "$output/armada/"
cp -- "$work/rotate-source.png" "$work/rotate-result.png" "$output/rotate/"
