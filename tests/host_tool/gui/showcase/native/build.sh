#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 3 ]; then
	echo "usage: $0 SOURCE-ROOT BUILD-DIRECTORY PREPARED-INPUTS-DIRECTORY" >&2
	exit 2
fi

root=$1
work=$2
screens=$root/tests/host_tool/gui/screens
peer=$root/tests/host_tool/gui/showcase/native
aport=$root/alpine/aports/fplinux-showcase
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$3"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check -o "$work/locale/ru/LC_MESSAGES/org.fplinux.showcase.mo" "$aport/ru.po"
msgfmt --check -o "$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
ui_host_compile_lvgl -O2 -I"$aport" \
	-Dclock_gettime=showcase_observer_clock_gettime \
	-c "$aport/fplinux-showcase.c" -o "$work/showcase-app.o"
ui_host_build_application -I"$aport" \
	"$work/showcase-app.o" "$aport/armada-scene.c" \
	"$aport/showcase-setup.c" "$aport/showcase-results.c" \
	"$aport/armada-storyboard.c" "$aport/armada-renderer.c" \
	"$aport/armada-geometry.c" "$ui_host_shared/fplinux-system-info.c" \
	"$peer/observer.c" "$screens/native-glyph-resources.c" \
	-Wl,--wrap=open \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=fplinux_ui_canvas_new \
	-Wl,--wrap=fplinux_ui_close \
	-Wl,--wrap=fplinux_ui_frame_new -Wl,--wrap=fplinux_ui_frame_delete \
	-Wl,--wrap=fplinux_ui_frame_present \
	-Wl,--wrap=fplinux_ui_now_ms \
	-Wl,--wrap=fopen \
	-Wl,--wrap=armada_scene_create_with_renderer \
	-Wl,--wrap=armada_scene_render -Wl,--wrap=armada_scene_destroy \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=lv_display_flush_ready -o "$work/showcase"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$ui_host_inputs" -I"$screens" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$peer/screens.c" "$screens/ui-frame-peer.c" "$screens/screen-frame.c" \
	"$ui_host_ui/fplinux-ui-session.c" "$ui_host_inputs/xdg-shell-protocol.c" \
	"$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_ui/fplinux-ui-profile.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/showcase-screens"
