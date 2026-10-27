#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$1
work=$2
screens=$root/site/scripts/screens
fixtures=$root/tests/host_tool/mre/fixtures
aport=$root/alpine/aports/fplinux-mre
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$3"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.mre.mo" "$aport/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"

# shellcheck disable=SC2046
ui_host_build_application -I"$aport" \
	"$aport/fplinux-mre.c" "$aport/runtime-paths.c" "$aport/vfs-broker.c" \
	"$aport/text-codec.c" "$aport/http-broker.c" "$fixtures/adapter.c" \
	-Wl,--wrap=execl -Wl,--wrap=fplinux_language_load \
	-Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=lv_timer_create \
	-Wl,--wrap=lv_timer_pause -Wl,--wrap=lv_timer_resume \
	$(pkg-config --cflags --libs libcurl libcrypto) -o "$work/mre"
# The real engine address-space limit cannot contain ASan's shadow mapping.
ui_host_compile -fno-sanitize=all -I"$aport" "$fixtures/engine-peer.c" -o "$work/engine-peer"
# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$ui_host_inputs" -I"$screens" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$fixtures/screens.c" "$screens/ui-frame-peer.c" \
	"$ui_host_ui/fplinux-ui-session.c" "$ui_host_ui/fplinux-ui-profile.c" \
	"$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/mre-screens"
