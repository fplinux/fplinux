#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
native=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$native/../../../../.." && pwd)
screens=$root/tests/host_tool/gui/screens
music=$root/alpine/aports/fplinux-music
media=$root/lib/fplinux-media
ui=$root/lib/fplinux
work=$1
inputs=$2
mpg123=$3
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$inputs"

# pkg-config supplies the flags for the pinned decoder and protocol libraries.
# shellcheck disable=SC2046
ui_host_build_application -I"$music" -I"$mpg123/src/include" \
	"$music/fplinux-music.c" "$music/music-player.c" \
	"$music/music-model.c" "$music/music-view.c" \
	"$media/fplinux-media.c" "$media/fplinux-media-amr-nb.c" \
	"$mpg123/src/libmpg123/.libs/libmpg123.a" \
	$(pkg-config --cflags --libs libavcodec-fplinux-amr libavutil-fplinux-amr) \
	-o "$work/music"
# shellcheck disable=SC2046
ui_host_compile -I"$screens" -I"$root/include/fplinux" -I"$inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$native/music-state-peer.c" "$screens/ui-frame-peer.c" \
	"$ui/fplinux-ui-profile.c" "$ui/fplinux-ui-session.c" \
	"$ui/fplinux-ui-prefs.c" "$ui/fplinux-utf8.c" \
	"$inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/music-state-peer"
