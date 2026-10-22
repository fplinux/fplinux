#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Clock and its native capture peer in the pinned environment.
set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
clocks=$root/alpine/aports/fplinux-clocks
session=$root/alpine/aports/fplinux-session
ui=$root/lib/fplinux
work=$1
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"
ui_host_prepare "$root" "$work" "${2-}"

ui_host_build_application -I"$clocks" \
	"$clocks/fplinux-clocks.c" "$clocks/clock-client.c" \
	"$clocks/clock-model.c" "$clocks/clock-wire.c" \
	"$clocks/clock-presentation.c" \
	"$screens_dir/clocks-observer.c" \
	-Wl,--wrap=lv_display_flush_ready \
	-Wl,--wrap=fplinux_ui_form_new \
	-Wl,--wrap=fplinux_ui_form_delete_all \
	-Wl,--wrap=fplinux_ui_form_append_date \
	-Wl,--wrap=fplinux_ui_form_append_duration -o "$work/clocks"

# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$clocks" -I"$session" -I"$ui_host_inputs" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$screens_dir/clocks-screens.c" \
	"$clocks/clock-model.c" "$clocks/clock-wire.c" \
	"$session/session-clock.c" "$ui/fplinux-ui-prefs.c" \
	"$ui/fplinux-utf8.c" \
	"$screens_dir/ui-frame-peer.c" "$screens_dir/screen-frame.c" \
	"$ui/fplinux-ui-session.c" "$ui/fplinux-ui-profile.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) \
	-Wl,--wrap=fsync -lm -o "$work/clocks-screens"
