#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
native=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$native/../../../../.." && pwd)
screens=$root/tests/host_tool/gui/screens
music=$root/alpine/aports/fplinux-music
ui=$root/lib/fplinux
work=$1
inputs=$2
shift 2
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$inputs"

ui_host_build_application -I"$music" \
	"$music/fplinux-music.c" "$music/music-player.c" \
	"$music/music-model.c" "$music/music-view.c" "$@" -o "$work/music"
# shellcheck disable=SC2046
ui_host_compile -I"$screens" -I"$root/include/fplinux" -I"$inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$native/music-state-peer.c" "$screens/ui-frame-peer.c" \
	"$ui/fplinux-ui-profile.c" "$ui/fplinux-ui-session.c" \
	"$ui/fplinux-ui-prefs.c" "$ui/fplinux-utf8.c" \
	"$inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/music-state-peer"
