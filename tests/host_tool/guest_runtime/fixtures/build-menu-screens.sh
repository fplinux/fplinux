#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$1
work=$2
inputs=$3
screens=$root/tests/host_tool/gui/screens
fixtures=$root/tests/host_tool/guest_runtime/fixtures
ui=$root/lib/fplinux
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"

# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$inputs" -I"$screens" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$fixtures/menu-screens.c" "$screens/ui-frame-peer.c" "$screens/screen-frame.c" \
	"$ui/fplinux-ui-session.c" "$ui/fplinux-ui-profile.c" \
	"$ui/fplinux-utf8.c" "$inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/menu-screens"
