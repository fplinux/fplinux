#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$1
work=$2
screens=$root/tests/host_tool/gui/screens
shared=$root/tests/host_tool/guest_runtime/fixtures
fixtures=$root/tests/host_tool/phoneme/native
aport=$root/alpine/aports/fplinux-phoneme
# shellcheck source=tests/host_tool/gui/screens/ui-host-build.sh
. "$screens/ui-host-build.sh"
ui_host_prepare "$root" "$work" "$3"

mkdir -p "$work/locale/ru/LC_MESSAGES"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/org.fplinux.phoneme.mo" "$aport/port/app/ru.po"
msgfmt --check --output-file="$work/locale/ru/LC_MESSAGES/fplinux-ui.mo" "$root/alpine/aports/fplinux-ui/ru.po"
# shellcheck disable=SC2046
ui_host_build_application -I"$aport/port/app" -I"$aport/vm" \
	"$aport/port/app/fplinux-pm-app.c" "$aport/port/app/fplinux-pm-lifecycle.c" \
	"$aport/port/app/fplinux-pm-store.c" "$aport/vm/fplinux-pm-jar.c" \
	"$fixtures/vm-peer.c" "$shared/ui-observer.c" \
	"$screens/native-glyph-resources.c" \
	-Wl,--wrap=open -Wl,--wrap=fsync -Wl,--wrap=fplinux_language_load \
	-Wl,--wrap=libintl_bindtextdomain -Wl,--wrap=lv_display_flush_ready \
	$(pkg-config --cflags --libs libcrypto) -lz -o "$work/phoneme"
sh "$shared/build-menu-screens.sh" "$root" "$work" "$3"
