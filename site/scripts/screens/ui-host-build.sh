#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Shared native UI build inputs; the capture scenario owns its workspace.

ui_host_prepare_inputs() {
	ui_host_root=$1
	ui_host_ui=$ui_host_root/lib/fplinux
	mkdir -p -- "$2"
	ui_host_inputs=$(CDPATH='' cd -- "$2" && pwd)
	ui_host_protocol=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
	wayland-scanner client-header "$ui_host_protocol" "$ui_host_inputs/xdg-shell-client-protocol.h"
	wayland-scanner server-header "$ui_host_protocol" "$ui_host_inputs/xdg-shell-server-protocol.h"
	wayland-scanner private-code "$ui_host_protocol" "$ui_host_inputs/xdg-shell-protocol.c"
	if [ "$#" -eq 4 ]; then
		ln -sfn -- "$3" "$ui_host_inputs/lvgl-headers"
		ln -sfn -- "$4" "$ui_host_inputs/liblvgl.a"
	else
		sh "$ui_host_root/scripts/build-ui-lvgl.sh" "$ui_host_inputs/lvgl"
		ln -sfn -- "$ui_host_inputs/lvgl/source/include" "$ui_host_inputs/lvgl-headers"
		ln -sfn -- "$ui_host_inputs/lvgl/build/liblvgl.a" "$ui_host_inputs/liblvgl.a"
	fi
	python3 "$ui_host_root/scripts/fplinux_ui_fonts.py" \
		--tokens "$ui_host_ui/fplinux-ui-profile.json" \
		--sources /opt/quality/src/ui-fonts --output "$ui_host_inputs/fonts" \
		--lv-font-conv /opt/quality/node-tools/node_modules/.bin/lv_font_conv
}

ui_host_prepare() {
	ui_host_root=$1
	ui_host_work=$2
	ui_host_ui=$ui_host_root/lib/fplinux
	mkdir -p -- "$ui_host_work"
	if [ -n "${3-}" ]; then
		ui_host_inputs=$3
	else
		ui_host_prepare_inputs "$ui_host_root" "$ui_host_work/dependencies"
	fi
}

ui_host_compile() {
	cc -std=c11 -Wall -Wextra -Werror "$@"
}

ui_host_compile_lvgl() {
	ui_host_compile -I"$ui_host_root/include" -I"$ui_host_root/include/fplinux" \
		-I"$ui_host_inputs" -isystem "$ui_host_inputs/lvgl-headers" \
		-DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SYSTEM \
		-DLV_CONF_PATH="\"$ui_host_root/alpine/aports/fplinux-lvgl/lv_conf.h\"" "$@"
}

ui_host_build_application() {
	# pkg-config emits compiler/linker arguments for the pinned libraries.
	# shellcheck disable=SC2046
	ui_host_compile_lvgl -O2 \
		$(pkg-config --cflags wayland-client xkbcommon json-c) \
		"$ui_host_ui/fplinux-ui.c" "$ui_host_ui/fplinux-ui-session.c" \
		"$ui_host_ui/fplinux-ui-style.c" \
		"$ui_host_ui/fplinux-ui-frame.c" \
		"$ui_host_ui/fplinux-audio-client.c" \
		"$ui_host_ui/fplinux-ui-text.c" "$ui_host_ui/fplinux-ui-text-model.c" \
		"$ui_host_ui/fplinux-ui-symbol-picker.c" "$ui_host_ui/fplinux-text-input.c" \
		"$ui_host_ui/fplinux-ui-menu.c" "$ui_host_ui/fplinux-ui-screen.c" \
		"$ui_host_ui/fplinux-ui-prefs.c" "$ui_host_ui/fplinux-ui-screen-model.c" \
		"$ui_host_ui/fplinux-ui-listview.c" "$ui_host_ui/fplinux-ui-locale.c" \
		"$ui_host_ui/fplinux-ui-profile.c" "$ui_host_ui/fplinux-ui-softkey-layout.c" \
		"$ui_host_ui/fplinux-cli.c" "$ui_host_ui/fplinux-multitap.c" \
		"$ui_host_ui/fplinux-language.c" "$ui_host_ui/fplinux-utf8.c" \
		"$@" \
		"$ui_host_inputs/xdg-shell-protocol.c" "$ui_host_inputs/fonts"/*.c \
		"$ui_host_inputs/liblvgl.a" \
		$(pkg-config --libs wayland-client xkbcommon json-c) -lintl -lm
}

if [ "${0##*/}" = ui-host-build.sh ]; then
	set -eu
	if [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; then
		echo "usage: $0 PREPARED-INPUTS-DIRECTORY [LVGL-HEADERS LVGL-ARCHIVE]" >&2
		exit 2
	fi
	ui_host_screens=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
	ui_host_source_root=$(CDPATH='' cd -- "$ui_host_screens/../../.." && pwd)
	ui_host_prepare_inputs "$ui_host_source_root" "$@"
fi
