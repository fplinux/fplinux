#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the application and its native scenario in the pinned environment.
set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
apidemo=$root/alpine/aports/fplinux-example-apidemo
work=$1
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"
ui_host_prepare "$root" "$work" "${2-}"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.example.apidemo.mo" "$apidemo/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
ui_host_build_application -I"$apidemo" \
	-DAPIDEMO_IMAGE_DIR="\"$apidemo/\"" \
	"$apidemo"/*.c "$screens_dir/apidemo-observer.c" \
	-Wl,--wrap=lv_label_set_text -Wl,--wrap=libintl_bindtextdomain \
	-Wl,--wrap=lv_label_set_text_fmt -Wl,--wrap=fplinux_ui_text_set \
	-Wl,--wrap=fplinux_language_load \
	-Wl,--wrap=fplinux_ui_list_new -Wl,--wrap=fplinux_ui_list_new_virtual \
	-Wl,--wrap=fplinux_ui_form_new -Wl,--wrap=fplinux_ui_alert_new \
	-Wl,--wrap=fplinux_ui_text_box_new \
	-Wl,--wrap=fplinux_ui_form_append_string -Wl,--wrap=fplinux_ui_form_set_string \
	-Wl,--wrap=fplinux_ui_prefs_save \
	-Wl,--wrap=open -Wl,--wrap=fplinux_ui_session_send \
	-Wl,--wrap=fplinux_ui_canvas_new -o "$work/apidemo"

# pkg-config emits flags for the pinned native peer libraries.
# shellcheck disable=SC2046
ui_host_compile_lvgl \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$screens_dir/apidemo-screens.c" "$screens_dir/ui-frame-peer.c" \
	"$screens_dir/screen-frame.c" "$ui_host_ui/fplinux-ui-session.c" \
	"$ui_host_ui/fplinux-ui-profile.c" "$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) \
	-o "$work/apidemo-screens"
