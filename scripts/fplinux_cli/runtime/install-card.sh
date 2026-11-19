#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu
umask 077

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

card_host_public_key() {
	key_details=$(/usr/bin/dropbearkey -y -f "$1" 2>/dev/null) || return 1
	printf '%s\n' "$key_details" | busybox awk '$1 == "ssh-ed25519" { print $2 }'
}

if [ "$#" -eq 2 ] && [ "$1" = prepare-access ]; then
	[ "$2" = "${0%/*}" ] || fail "Invalid system card staging directory"
	/usr/bin/dropbearkey -t ed25519 -f "$2/host-key" >/dev/null 2>&1 ||
		fail "Cannot generate a system card SSH host key"
	chmod 0600 "$2/host-key"
	card_host_public_key "$2/host-key" || fail "Cannot read the system card SSH host key"
	exit 0
fi

[ "$#" -eq 15 ] || fail "Invalid release card writer arguments"
device=$1
cid=$2
staging=$3
boot_start=$4
boot_mib=$5
root_start=$6
root_mib=$7
boot_hash=$8
root_hash=$9
fit_name=${10}
board_hash=${11}
personal_hash=${12}
checksums_hash=${13}
fit_hash=${14}
host_key_body=${15}

case "$device" in
/dev/mmcblk[0-9] | /dev/mmcblk[0-9][0-9]) ;;
*) fail "Expected a whole SD card device" ;;
esac
case "$cid" in
'' | *[!0-9a-f]*) fail "Invalid SD card CID" ;;
esac
[ "${#cid}" -eq 32 ] || fail "Invalid SD card CID"
for mib in "$boot_start" "$boot_mib" "$root_start" "$root_mib"; do
	case "$mib" in
	'' | 0* | *[!0-9]*) fail "Invalid release card layout" ;;
	esac
done
[ "$((boot_start + boot_mib))" -eq "$root_start" ] &&
	[ "$((root_start + root_mib))" -le 4096 ] || fail "Invalid release card layout"
[ "$fit_name" = FPLINUX.ITB ] || fail "Invalid release card FIT filename"

slot=/sys/block/${device##*/}
boot_partition=${device}p1
root_partition=${device}p2
minimum_sectors=$(((root_start + root_mib) * 2048))

card_progress() {
	/usr/libexec/fplinux/preparation --stage "$1" \
		--completed "$(($2 * 1048576))" --total "$((minimum_sectors * 512))" \
		/run/fplinux-preparation-status 2>/dev/null || :
}

check_card() {
	[ -f /etc/fplinux/preparation ] && [ "$(awk '$2 == "/" { print $3 }' /proc/mounts)" = overlay ] ||
		fail "Boot the FPLinux RAM reader before installing to the card"
	# MMC disks appear and disappear as a whole, so the card type identifies microSD.
	[ -b "$device" ] && [ "$(cat "$slot/device/type")" = SD ] ||
		fail "The selected SD card is no longer present"
	[ "$(cat "$slot/device/cid")" = "$cid" ] || fail "The SD card changed"
	[ "$(cat "$slot/ro")" = 0 ] || fail "The selected SD card is read-only"
	[ "$(cat "$slot/queue/logical_block_size")" = 512 ] ||
		fail "The card must use 512-byte sectors"
	[ "$(cat "$slot/size")" -ge "$minimum_sectors" ] || fail "The selected card is too small"
	awk -v device="$device" '
		$1 == device || index($1, device "p") == 1 { busy = 1 }
		END { exit busy }
	' /proc/mounts /proc/swaps || fail "Unmount the SD card and release its swap before installation"
}

check_partitions() {
	check_card
	check_partition "$boot_partition" "$boot_start" "$boot_mib"
	check_partition "$root_partition" "$root_start" "$root_mib"
}

check_partition() {
	partition_slot=$slot/${1##*/}
	[ -b "$1" ] && [ "$(cat "$partition_slot/start")" = "$(($2 * 2048))" ] &&
		[ "$(cat "$partition_slot/size")" = "$(($3 * 2048))" ] ||
		fail "The SD card partition layout does not match the release"
}

stream_source=
stream_sink=
mounted=
work=
card_writing=0

cleanup() {
	status=$?
	trap - EXIT
	trap '' HUP INT TERM
	for child in "$stream_sink" "$stream_source"; do
		if [ -n "$child" ]; then
			kill "$child" 2>/dev/null || :
			wait "$child" 2>/dev/null || :
		fi
	done
	if [ "$card_writing" -eq 1 ]; then
		busybox sync || printf '%s\n' "Cannot sync the SD card" >&2
	fi
	if [ -n "$mounted" ] && busybox mountpoint -q "$mounted"; then
		busybox umount "$mounted" || printf '%s\n' "Cannot unmount the SD card; keep the phone on" >&2
	fi
	if [ -n "$work" ]; then
		rm -f "$work/stream" "$work/result" || :
		rmdir "$work/mount" "$work" 2>/dev/null || :
	fi
	exit "$status"
}

start_stream() {
	"$@" >"$work/stream" &
	stream_source=$!
}

consume_stream() {
	"$@" <"$work/stream" >"$work/result" &
	stream_sink=$!
	if wait "$stream_sink"; then
		stream_sink=
	else
		stream_sink=
		return 1
	fi
	if wait "$stream_source"; then
		stream_source=
	else
		stream_source=
		return 1
	fi
}

check_digest() {
	digest_line=$(busybox sha256sum "$1") && [ "${digest_line%% *}" = "$2" ]
}

check_stream_digest() {
	consume_stream busybox sha256sum && read -r digest_line <"$work/result" &&
		[ "${digest_line%% *}" = "$1" ]
}

mount_partition() {
	mounted=$work/mount
	busybox mount "$@" "$mounted" || fail "Cannot mount the system card partition"
}

unmount_partition() {
	busybox umount "$mounted" || fail "Cannot unmount the SD card; keep the phone on"
	mounted=
}

verify_installed_files() (
	cd "$mounted" || exit 1
	if [ -s "$staging/installed.sha256" ]; then
		busybox sha256sum -cs "$staging/installed.sha256" || exit 1
	fi
	busybox tar -tf "$staging/personal.tar" >"$work/result" || exit 1
	while IFS= read -r path; do
		[ "$(busybox stat -c %a:%u:%g "$path")" = 600:0:0 ] || exit 1
	done <"$work/result"
)

verify_card_access_file() {
	[ -f "$1" ] && [ ! -L "$1" ] && [ "$(busybox stat -c %a:%u:%g "$1")" = 600:0:0 ]
}

install_card_access() {
	access_dir=$mounted/etc/fplinux/card-access
	mkdir -m 0700 "$access_dir"
	for access_file in device-id usb-network client-key host-key; do
		busybox cp "$staging/$access_file" "$access_dir/$access_file"
		chmod 0600 "$access_dir/$access_file"
	done
}

verify_card_access() {
	access_dir=$mounted/etc/fplinux/card-access
	[ -d "$access_dir" ] && [ ! -L "$access_dir" ] &&
		[ "$(busybox stat -c %a:%u:%g "$access_dir")" = 700:0:0 ] || return 1
	for access_file in device-id usb-network client-key host-key; do
		verify_card_access_file "$access_dir/$access_file" &&
			busybox cmp -s "$staging/$access_file" "$access_dir/$access_file" || return 1
	done
}

exec 9>/run/fplinux-card-install.lock
busybox flock -n 9 || fail "Another system card installation is running in this reader"
check_card
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$staging/work.XXXXXXXX")
mkdir "$work/mount"
mkfifo "$work/stream"

check_digest "$staging/board.tar" "$board_hash" || fail "The board archive failed verification"
check_digest "$staging/personal.tar" "$personal_hash" || fail "The personal archive failed verification"
check_digest "$staging/installed.sha256" "$checksums_hash" || fail "The installed file checksums failed verification"
check_digest "$staging/fit.itb" "$fit_hash" || fail "The personalized FIT failed verification"
[ "$(wc -c <"$staging/device-id")" -eq 32 ] &&
	[ "$(wc -c <"$staging/usb-network")" -eq 256 ] &&
	[ "$(wc -c <"$staging/client-key")" -eq 68 ] || fail "Invalid system card access files"
verify_card_access_file "$staging/host-key" && [ "${#host_key_body}" -eq 68 ] &&
	[ "$(card_host_public_key "$staging/host-key")" = "$host_key_body" ] || fail "The card SSH host key failed verification"
start_stream busybox xzcat "$staging/boot.xz"
check_stream_digest "$boot_hash" || fail "The boot image failed verification"
start_stream busybox xzcat "$staging/root.ext4.xz"
check_stream_digest "$root_hash" || fail "The ext4 image failed verification"

check_card
card_writing=1
card_progress card-writing 0
start_stream busybox xzcat "$staging/boot.xz"
consume_stream busybox dd of="$device" bs=1048576 count="$root_start" iflag=fullblock conv=notrunc status=none ||
	fail "Cannot write the boot image to the SD card"
card_progress card-writing "$root_start"
check_card
start_stream busybox xzcat "$staging/root.ext4.xz"
consume_stream busybox dd of="$device" bs=1048576 seek="$root_start" count="$root_mib" iflag=fullblock conv=notrunc status=none ||
	fail "Cannot write the ext4 image to the SD card"
busybox sync
card_progress card-writing "$((root_start + root_mib))"
check_card
busybox blockdev --flushbufs "$device" || fail "Cannot flush the SD card before readback"
card_progress card-verifying 0
start_stream busybox dd if="$device" bs=1048576 count="$root_start" status=none
check_stream_digest "$boot_hash" || fail "The SD card boot bytes do not match the release"
card_progress card-verifying "$root_start"
start_stream busybox dd if="$device" bs=1048576 skip="$root_start" count="$root_mib" status=none
check_stream_digest "$root_hash" || fail "The SD card ext4 bytes do not match the release"
card_progress card-verifying "$((root_start + root_mib))"

busybox blockdev --rereadpt "$device" || fail "Cannot reread the SD card partition table"
check_partitions
mount_partition -t ext4 -o rw,nosuid,nodev,noexec "$root_partition"
(
	# Tar can create leading directories. Use the normal rootfs directory mode.
	umask 022
	busybox tar --numeric-owner -xpf "$staging/board.tar" -C "$mounted" ||
		fail "Cannot install the board files on the SD card"
	busybox tar --numeric-owner -xkpf "$staging/personal.tar" -C "$mounted" ||
		fail "Cannot install this phone's firmware on the SD card"
)
install_card_access
busybox sync
unmount_partition
check_partitions
busybox blockdev --flushbufs "$root_partition" || fail "Cannot flush the SD card root filesystem"
mount_partition -t ext4 -o ro,noload,nosuid,nodev,noexec "$root_partition"
verify_installed_files || fail "The SD card root files or firmware permissions do not match"
verify_card_access || fail "The SD card SSH access files or permissions do not match"
unmount_partition

check_partitions
mount_partition -t vfat -o rw,nosuid,nodev,noexec "$boot_partition"
[ ! -e "$mounted/$fit_name" ] || fail "The release boot filesystem already contains a FIT"
busybox cp "$staging/fit.itb" "$mounted/$fit_name" || fail "Cannot write the FIT to the SD card"
busybox sync
unmount_partition
check_partitions
busybox blockdev --flushbufs "$boot_partition" || fail "Cannot flush the SD card boot filesystem"
mount_partition -t vfat -o ro,nosuid,nodev,noexec "$boot_partition"
check_digest "$mounted/$fit_name" "$fit_hash" || fail "The SD card FIT does not match the phone's FIT"
unmount_partition
check_card
card_writing=0
