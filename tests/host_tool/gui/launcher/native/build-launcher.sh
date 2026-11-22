#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Launcher and its controlled native session peer.
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY PREPARED-INPUTS-DIRECTORY" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../../../.." && pwd)
common=$root/tests/host_tool/gui/screens
launcher=$root/alpine/aports/fplinux-launcher
work=$1
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$common/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$2"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.launcher.mo" "$launcher/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"

# pkg-config emits compiler and linker arguments for the pinned libraries.
# shellcheck disable=SC2046
ui_host_build_application -I"$common" -I"$launcher" $(pkg-config --cflags dbus-1) \
	"$launcher"/*.c "$ui_host_shared/fplinux-clock-model.c" "$ui_host_shared/fplinux-clock-wire.c" \
	"$ui_host_shared/clock-face.c" \
	"$ui_host_shared/fplinux-desktop-entry.c" \
	"$ui_host_shared/fplinux-bluez-adapter.c" \
	"$root/tests/host_tool/gui/launcher/native/launcher-resources.c" \
	"$root/tests/host_tool/gui/launcher/native/launcher-observer.c" \
	-Wl,--wrap=open -Wl,--wrap=inotify_add_watch \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=lv_timer_create \
	-Wl,--wrap=lv_timer_set_period -Wl,--wrap=lv_timer_delete \
	-Wl,--wrap=lv_display_flush_ready -Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=time \
	-Wl,--wrap=clock_gettime \
	$(pkg-config --libs dbus-1) -o "$work/launcher"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/tests/host_tool/gui/support" -I"$common" -I"$root/include/fplinux" -I"$ui_host_inputs" -I"$screens_dir" \
	$(pkg-config --cflags wayland-server xkbcommon json-c dbus-1) \
	"$root/tests/host_tool/gui/launcher/native/launcher-screens.c" "$common/ui-frame-peer.c" \
	"$root/tests/host_tool/gui/support/bluez-fake.c" \
	"$common/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-utf8.c" "$ui_host_shared/fplinux-clock-model.c" "$ui_host_shared/fplinux-clock-wire.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c dbus-1) \
	-o "$work/launcher-screens"

session=$root/alpine/aports/fplinux-session
# The manager and Launcher share their real socket; only audio replies are controlled.
# shellcheck disable=SC2046
ui_host_compile -pthread -I"$common" -I"$screens_dir" -I"$session" \
	-I"$root/include" -I"$root/include/fplinux" -I"$ui_host_inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c dbus-1) \
	"$screens_dir/launcher-session-volume.c" "$common/ui-frame-peer.c" \
	"$session/session-core.c" "$session/session-child.c" "$session/session-bluetooth.c" \
	"$session/session-storage.c" "$session/session-audio.c" "$session/session-volume.c" \
	"$ui_host_shared/fplinux-audio-control.c" "$ui_host_shared/fplinux-audio-status.c" \
	"$session/session-vibration.c" "$session/session-vibrator.c" \
	"$session/session-power.c" "$session/session-settings.c" \
	"$session/session-battery.c" "$session/session-clock.c" \
	"$session/session-wireless.c" \
	"$session/session-wifi-station.c" "$session/session-wifi-dhcp.c" \
	"$ui_host_shared/fplinux-settings-values.c" \
	"$ui_host_shared/fplinux-clock-model.c" "$ui_host_shared/fplinux-clock-wire.c" \
	"$ui_host_shared/fplinux-brightness-client.c" "$ui_host_shared/fplinux-brightness-wire.c" \
	"$ui_host_shared/fplinux-keypad-light.c" \
	"$ui_host_shared/fplinux-desktop-entry.c" "$ui_host_ui/fplinux-ui-prefs.c" \
	"$ui_host_ui/fplinux-ui-profile.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-language.c" "$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	-Wl,--wrap=fplinux_session_wireless_create \
	$(pkg-config --libs wayland-server xkbcommon json-c dbus-1) -lintl \
	-o "$work/launcher-session-volume"
