#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Settings and its native session and capture peer.
set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
settings=$root/alpine/aports/fplinux-settings
work=$1
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"
ui_host_prepare "$root" "$work" "${2-}"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.settings.mo" "$settings/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
ui_host_build_application -I"$settings" \
	"$settings/fplinux-settings.c" "$settings/settings-core.c" \
	"$ui_host_ui/fplinux-settings-values.c" \
	"$screens_dir/settings-observer.c" \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=lv_display_flush_ready \
	-Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=fplinux_language_load -Wl,--wrap=fplinux_language_list \
	-o "$work/settings"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$ui_host_inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$screens_dir/settings-screens.c" "$screens_dir/ui-frame-peer.c" \
	"$screens_dir/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-ui-profile.c" "$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) \
	-o "$work/settings-screens"
