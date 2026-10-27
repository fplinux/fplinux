#!/bin/sh
# SPDX-License-Identifier: 0BSD
set -eu

if [ "$#" -ne 3 ]; then
	echo "usage: $0 SOURCE-ROOT BUILD-DIRECTORY PREPARED-INPUTS-DIRECTORY" >&2
	exit 2
fi

root=$1
work=$2
screens=$root/site/scripts/screens
peer=$root/tests/host_tool/gui/notes-native
aport=$root/alpine/aports/fplinux-notes
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$3"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.notes.mo" "$aport/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
ui_host_build_application "$aport/fplinux-notes.c" "$peer/observer.c" \
	-Wl,--wrap=fplinux_ui_open -Wl,--wrap=fplinux_ui_text_box_new \
	-Wl,--wrap=fplinux_language_load \
	-Wl,--wrap=libintl_bindtextdomain -Wl,--wrap=lv_display_flush_ready \
	-o "$work/notes"

# pkg-config emits compiler and linker arguments for the pinned peer libraries.
# shellcheck disable=SC2046
ui_host_compile -I"$root/include/fplinux" -I"$ui_host_inputs" -I"$screens" \
	$(pkg-config --cflags wayland-server xkbcommon json-c) \
	"$peer/screens.c" "$screens/ui-frame-peer.c" "$screens/screen-frame.c" \
	"$ui_host_ui/fplinux-ui-session.c" "$ui_host_ui/fplinux-ui-profile.c" \
	"$ui_host_ui/fplinux-utf8.c" \
	"$ui_host_inputs/xdg-shell-protocol.c" \
	$(pkg-config --libs wayland-server xkbcommon json-c) -o "$work/notes-screens"
