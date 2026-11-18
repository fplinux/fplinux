#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../../../.." && pwd)
common=$root/tests/host_tool/gui/screens
music=$root/alpine/aports/fplinux-music
work=$1
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$common/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$2"
mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check -o "$work/locale/ru/LC_MESSAGES/org.fplinux.music.mo" "$music/ru.po"
msgfmt --check -o "$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
ui_host_build_application -I"$music" \
	"$screens_dir/music-view-app.c" "$music/music-view.c" \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=libintl_bindtextdomain \
	-o "$work/music-view"
# pkg-config supplies argument lists for the pinned protocol libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$common" -I"$root/include/fplinux" -I"$ui_host_inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$screens_dir/music-view-screens.c" "$common/ui-frame-peer.c" \
	"$common/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-utf8.c" "$ui_host_ui/fplinux-ui-profile.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/music-view-screens"
