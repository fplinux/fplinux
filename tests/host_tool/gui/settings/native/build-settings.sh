#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Settings and its native session and capture peer.
set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../../../.." && pwd)
common=$root/tests/host_tool/gui/screens
settings=$root/alpine/aports/fplinux-settings
work=$1
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$common/ui-host-build.sh"
ui_host_prepare "$root" "$work" "${2-}"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.settings.mo" "$settings/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
# pkg-config emits compiler and linker arguments for the pinned libraries.
# shellcheck disable=SC2046
ui_host_build_application -pthread -I"$common" -I"$settings" $(pkg-config --cflags dbus-1) \
	"$settings/fplinux-settings.c" "$settings/settings-core.c" \
	"$settings/settings-bluetooth.c" \
	"$settings/settings-wifi.c" \
	"$ui_host_shared/fplinux-settings-values.c" \
	"$ui_host_shared/fplinux-system-info.c" \
	"$ui_host_shared/fplinux-bluez-adapter.c" \
	"$ui_host_shared/fplinux-bluetooth-common.c" \
	"$ui_host_shared/fplinux-bluetooth-opp.c" \
	"$screens_dir/settings-observer.c" \
	"$common/list-values-observer.c" \
	"$common/native-glyph-resources.c" \
	-Wl,--wrap=open \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=lv_display_flush_ready \
	-Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=fplinux_system_info_read_os_name \
	-Wl,--wrap=fplinux_system_info_read_os_field \
	-Wl,--wrap=fplinux_system_info_read_line \
	-Wl,--wrap=fplinux_settings_read_memory \
	-Wl,--wrap=fplinux_settings_about_logo -Wl,--wrap=uname -Wl,--wrap=time \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=fplinux_language_list \
	$(pkg-config --libs dbus-1) -o "$work/settings"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/tests/host_tool/gui/support" -I"$common" -I"$root/include/fplinux" -I"$ui_host_inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c dbus-1) \
	"$screens_dir/settings-screens.c" "$common/ui-frame-peer.c" \
	"$root/tests/host_tool/gui/support/bluez-fake.c" \
	"$common/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_ui/fplinux-ui-profile.c" "$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c dbus-1) \
	-o "$work/settings-screens"

# The private peer shares the bus with the native BlueZ fixture.
# shellcheck disable=SC2046
ui_host_compile $(pkg-config --cflags dbus-1) \
	-I"$root/tests/host_tool/gui/support" \
	"$root/tests/host_tool/media/fplinux-bluetooth-service.c" \
	"$root/tests/host_tool/gui/support/bluez-fake.c" \
	$(pkg-config --libs dbus-1) -o "$work/bluetooth-service"
