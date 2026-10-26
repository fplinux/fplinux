#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Capture the actual Terminal with a controlled shell over its real PTY.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: $0 OUTPUT-DIRECTORY [PREPARED-INPUTS-DIRECTORY]" >&2
	exit 2
fi

screens_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$screens_dir/../../.." && pwd)
terminal=$root/alpine/aports/fplinux-terminal
ui=$root/lib/fplinux
output=$1
prepared_inputs=${2-}
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens_dir/ui-host-build.sh"

build_application() {
	ui_host_prepare "$root" "$work" "$prepared_inputs"
	mkdir -p -- "$work/locale/ru/LC_MESSAGES"
	msgfmt --check --output-file "$work/locale/ru/LC_MESSAGES/org.fplinux.terminal.mo" \
		"$terminal/ru.po"
	msgfmt --check --output-file "$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" \
		"$root/alpine/aports/fplinux-ui/ru.po"
	ui_host_compile "$screens_dir/terminal-shell-peer.c" -o "$work/terminal-shell-peer"
	ui_host_compile "$terminal/terminal-pty-supervisor.c" -o "$work/terminal-pty-supervisor"
	# pkg-config emits compiler/linker arguments for the pinned libraries.
	# shellcheck disable=SC2046
	ui_host_build_application -I"$terminal" \
		-DFPLINUX_TERMINAL_PTY_HELPER_PATH="\"$work/terminal-pty-supervisor\"" \
		-DFPLINUX_SCREEN_SHELL_PATH="\"$work/terminal-shell-peer\"" \
		-DTERMINAL_FONT_DIRECTORY='"/opt/quality/fonts"' \
		$(pkg-config --cflags libtsm) \
		"$terminal/fplinux-terminal.c" "$terminal/terminal-engine.c" \
		"$terminal/terminal-help.c" "$terminal/terminal-input.c" \
		"$terminal/terminal-pty.c" "$terminal/terminal-render.c" \
		"$terminal/terminal-session.c" "$screens_dir/terminal-pty-peer.c" \
		"$screens_dir/terminal-observer.c" \
		"$ui/fplinux-font.c" \
		$(pkg-config --libs libtsm) -Wl,--wrap=fplinux_terminal_pty_open \
		-Wl,--wrap=fplinux_language_list -Wl,--wrap=fplinux_language_load \
		-Wl,--wrap=fplinux_ui_canvas_new -Wl,--wrap=fplinux_ui_close \
		-Wl,--wrap=libintl_bindtextdomain \
		-Wl,--wrap=fplinux_ui_list_new \
		-Wl,--wrap=fplinux_ui_watch_fd -Wl,--wrap=fplinux_terminal_pty_close \
		-Wl,--wrap=fplinux_terminal_render \
		-Wl,--wrap=wl_display_flush \
		-o "$work/terminal"
	# shellcheck disable=SC2046
	ui_host_compile -I"$root/include" -I"$root/include/fplinux" -I"$ui_host_inputs" \
		$(pkg-config --cflags wayland-server xkbcommon json-c) \
		"$screens_dir/terminal-screens.c" "$screens_dir/ui-frame-peer.c" \
		"$screens_dir/screen-frame.c" "$ui/fplinux-ui-session.c" \
		"$ui/fplinux-ui-profile.c" "$ui_host_inputs/xdg-shell-protocol.c" \
		$(pkg-config --libs wayland-server xkbcommon json-c) \
		-o "$work/terminal-screens"
}

capture() {
	width=$1
	height=$2
	suffix=$3
	for locale in en ru; do
		for theme in dark light; do
			frames=$work/$width"x"$height-$locale-$theme
			mkdir -- "$frames"
			"$work/terminal-screens" "$width" "$height" "$work/terminal" \
				"$screens_dir/keypad.xkb" "$work/session.sock" "$frames" \
				"$root/alpine/aports/fplinux-base/issue" \
				"$root/alpine/aports/fplinux-base/hostname" \
				"$root/alpine/aports/fplinux-language" "$theme" "$locale" "$work/locale"
			read -r frame_width frame_height <"$frames/frame-size"
			for frame in "$frames"/*.rgb565; do
				state=$(basename -- "$frame" .rgb565)
				python3 "$screens_dir/rgb565-png.py" "$frame_width" "$frame_height" \
					"$frame" "$output/$state$suffix.png"
				cp -- "$frames/$state.json" "$output/$state$suffix.json"
			done
		done
	done
}

mkdir -p -- "$output"
build_application
# Integrated surfaces exclude the manager's status and soft-key bars.
capture 128 128 -128x128
capture 128 160 -128x160
capture 176 220 -176x220
capture 240 320 ''
capture 320 240 -320x240
