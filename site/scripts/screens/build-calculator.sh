#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the actual Calculator and its native capture peer in the pinned environment.
set -eu

if [ "$#" -ne 1 ] && [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
calculator=$root/alpine/aports/fplinux-calculator
ui=$root/lib/fplinux
work=$1
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"
ui_host_prepare "$root" "$work" "${2-}"

ui_host_build_application -I"$calculator" \
	"$calculator/fplinux-calculator.c" "$calculator/calculator.c" \
	"$screens_dir/calculator-observer.c" \
	-Wl,--wrap=lv_label_set_text -Wl,--wrap=fplinux_ui_prefs_save \
	-Wl,--wrap=fplinux_ui_alert_new -o "$work/calculator"

# shellcheck disable=SC2046
ui_host_compile_lvgl -I"$calculator" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$screens_dir/calculator-screens.c" "$calculator/calculator.c" \
	"$screens_dir/ui-frame-peer.c" "$screens_dir/screen-frame.c" \
	"$ui/fplinux-ui-session.c" "$ui/fplinux-ui-profile.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) \
	-lm -o "$work/calculator-screens"
