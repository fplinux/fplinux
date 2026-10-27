#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Launcher and its controlled native session peer.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY PREPARED-INPUTS-DIRECTORY" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
launcher=$root/alpine/aports/fplinux-launcher
clocks=$root/alpine/aports/fplinux-clocks
work=$1
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$2"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.launcher.mo" "$launcher/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"

ui_host_build_application -I"$launcher" -I"$clocks" \
	"$launcher"/*.c "$clocks/clock-model.c" "$clocks/clock-wire.c" \
	"$clocks/clock-face.c" \
	"$ui_host_ui/fplinux-desktop-entry.c" \
	"$root/tests/host_tool/gui/launcher-resources.c" \
	"$root/tests/host_tool/gui/launcher-observer.c" \
	-Wl,--wrap=open -Wl,--wrap=inotify_add_watch \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=lv_timer_create \
	-Wl,--wrap=lv_timer_set_period -Wl,--wrap=lv_timer_delete \
	-Wl,--wrap=lv_display_flush_ready -Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=time \
	-Wl,--wrap=clock_gettime \
	-o "$work/launcher"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$ui_host_inputs" -I"$screens_dir" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$root/tests/host_tool/gui/launcher-screens.c" "$screens_dir/ui-frame-peer.c" \
	"$screens_dir/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) \
	-o "$work/launcher-screens"
