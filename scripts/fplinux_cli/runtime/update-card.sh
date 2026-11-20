#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The RAM reader owns the mounted card, the writer lock and the staged assets.

set -eu
umask 077

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

valid_sha256() {
	case "$1" in
	'' | *[!0-9a-f]*) return 1 ;;
	esac
	[ "${#1}" -eq 64 ]
}

check_digest() {
	[ -f "$1" ] && [ ! -L "$1" ] &&
		digest=$(busybox sha256sum "$1") && [ "${digest%% *}" = "$2" ]
}

flush_card() {
	busybox sync -f "$card" || fail "Cannot flush the system card"
}

slots() {
	slot_action=$1
	shift
	sh "${0%/*}/card-slots.sh" "$card" "$target" "$slot_action" "$build_id" "$@"
}

verify_board() {
	check_digest "$staging/board.sha256" "$board_checksums_sha" ||
		fail "The board file checksums failed verification"
	if [ -s "$staging/board.sha256" ]; then
		(cd "$1" && busybox sha256sum -cs "$staging/board.sha256") ||
			fail "The prepared board files failed verification"
	fi
}

stock_mount=

cleanup() {
	if [ -n "$stock_mount" ]; then
		if busybox mountpoint -q "$stock_mount"; then
			busybox umount "$stock_mount" || fail "Cannot unmount the stock filesystem"
		fi
		busybox rmdir "$stock_mount"
	fi
}

copy_stock_root() {
	stock_image=$staging/stock.ext4
	stock_checksums=$staging/stock.sha256
	busybox xzcat "$staging/root.ext4.xz" >"$stock_image" ||
		fail "Cannot unpack the stock ext4 image onto the card"
	flush_card
	check_digest "$stock_image" "$root_sha" || fail "The stock ext4 image failed verification"

	trap cleanup EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
	stock_mount=$(busybox mktemp -d /run/fplinux-stock.XXXXXXXX)
	busybox mount -t ext4 -o loop,ro,noload,nosuid,nodev,noexec "$stock_image" "$stock_mount" ||
		fail "Cannot mount the stock ext4 image"
	stock_root=$stock_mount/system/a/root
	busybox cp -a "$stock_root" "$1" || fail "Cannot copy the stock system root"
	(
		cd "$stock_root"
		busybox find . -type f -exec busybox sha256sum '{}' +
	) >"$stock_checksums" || fail "Cannot read the stock system files"
	flush_card
	(cd "$1" && busybox sha256sum -cs "$stock_checksums") ||
		fail "The copied system files differ from the stock ext4 image"

	cleanup
	stock_mount=
	busybox rm -f "${stock_image:?}" "${stock_checksums:?}"
}

[ "$#" -ge 5 ] || fail "Invalid system card update arguments"
action=$1
card=$2
target=$3
build_id=$4
fit_sha=$5
valid_sha256 "$fit_sha" || fail "Invalid personalized FIT digest"
case "$action:$#" in
inspect:5) ;;
prepare:8)
	root_sha=$6
	board_sha=$7
	board_checksums_sha=$8
	valid_sha256 "$root_sha" && valid_sha256 "$board_sha" && valid_sha256 "$board_checksums_sha" ||
		fail "Invalid stock image or board digest"
	;;
publish:6)
	board_checksums_sha=$6
	valid_sha256 "$board_checksums_sha" || fail "Invalid board checksums digest"
	;;
*) fail "Invalid system card update arguments" ;;
esac

selection=$(slots plan-update) || exit 1
case "$selection" in
'active a' | 'active b')
	check_digest "$card/system/${selection##* }/FPLINUX.ITB" "$fit_sha" ||
		fail "The active build has a different personalized FIT"
	if [ "$action" = inspect ]; then
		printf '%s\n' "$selection"
	else
		flush_card
	fi
	exit 0
	;;
'inactive a') previous=b ;;
'inactive b') previous=a ;;
*) fail "Invalid system card update slot" ;;
esac
if [ "$action" = inspect ]; then
	printf '%s\n' "$selection"
	exit 0
fi

staging=$card/system/.update
[ -d "$staging" ] && [ ! -L "$staging" ] || fail "The system card has no update staging directory"
pair=$staging/pair

if [ "$action" = prepare ]; then
	check_digest "$staging/fit.itb" "$fit_sha" || fail "The personalized FIT failed verification"
	check_digest "$staging/board.tar" "$board_sha" || fail "The board archive failed verification"
	[ -f "$staging/root.ext4.xz" ] && [ ! -L "$staging/root.ext4.xz" ] ||
		fail "The stock ext4 image is missing"

	incoming=$staging/pair.new
	busybox rm -rf -- "${incoming:?}" "${pair:?}"
	busybox mkdir -m 0755 "$incoming"
	copy_stock_root "$incoming/root"
	(
		# The board layer can create leading rootfs directories.
		umask 022
		busybox tar --numeric-owner -xpf "$staging/board.tar" -C "$incoming/root"
	) || fail "Cannot prepare the board files on the system card"
	busybox install -m 0644 "$incoming/root/etc/apk/world" "$incoming/base-world" ||
		fail "Cannot save the stock package selection"
	busybox cp "$staging/fit.itb" "$incoming/FPLINUX.ITB"
	flush_card
	verify_board "$incoming/root"
	check_digest "$incoming/FPLINUX.ITB" "$fit_sha" || fail "The prepared FIT failed verification"
	busybox mv -T "$incoming" "$pair"
else
	[ -f "$pair/base-world" ] && [ ! -L "$pair/base-world" ] ||
		fail "The prepared system has no stock package selection"
	# APK updates world and the package database, which can be supplied by the board layer.
	verify_board "$pair/root"
	sh "${0%/*}/card-apk-world.sh" restore "$card/system/$previous" "$pair/root" "$staging/apks"
	flush_card
	check_digest "$pair/FPLINUX.ITB" "$fit_sha" || fail "The prepared FIT failed verification"
	slots activate "$pair"
fi
