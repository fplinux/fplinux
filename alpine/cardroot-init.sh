#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

PATH=/bin
export PATH

card_backing=/run/fplinux-card
card_identity=/etc/fplinux-card
card_directories=/etc/fplinux/card-data-directories
card_writable=0

card_fail() {
	printf '<3>fplinux-card: %s\n' "$1" >/dev/kmsg
	printf 'fplinux-card: %s\n' "$1" >&2
	if [ "$card_writable" = 1 ]; then
		busybox mount -o remount,ro "$card_partition" "$card_backing" ||
			printf 'fplinux-card: cannot make the SD card read-only\n' >&2
	fi
	while :; do
		busybox sleep 3600
	done
}

find_card_partition() {
	card_partition=
	for card_block in /sys/block/mmcblk[0-9]*; do
		[ "$(busybox cat "$card_block/device/type" 2>/dev/null)" = SD ] || continue
		card_disk=/dev/${card_block##*/}
		card_p2=${card_disk}p2
		[ -b "$card_disk" ] && [ -b "$card_p2" ] || continue
		busybox cmp -s "$card_block/${card_p2##*/}/start" "$card_identity/root-start" ||
			continue
		busybox dd if="$card_disk" bs=1 skip=440 count=4 status=none |
			busybox cmp -s "$card_identity/disk-id" - || continue
		case $(busybox blkid "$card_p2") in
		*" UUID=\"$card_uuid\""*" TYPE=\"ext4\""*) ;;
		*) continue ;;
		esac
		[ -z "$card_partition" ] || card_fail "more than one SD card matches the system layout"
		card_partition=$card_p2
	done
	[ -n "$card_partition" ]
}

verify_card_root() {
	printf '%s\n' "$card_slot" | busybox cmp -s "$card_backing/system/active" - ||
		card_fail "card selector differs from the slot chosen by U-Boot"
	busybox cmp -s "$card_backing/system/target" "$card_identity/target" ||
		card_fail "system card belongs to a different phone model"
	card_root=$card_backing/system/$card_slot/root
	card_build_line=$(busybox grep '^BUILD_ID=' "$card_root/etc/os-release") ||
		card_fail "selected root has no build identity"
	[ "$card_build_line" = "BUILD_ID=$card_build_id" ] ||
		card_fail "selected root build identity differs from the FIT"
	[ -x "$card_root/sbin/init" ] || card_fail "system init is missing"
	while read -r card_path _card_mode; do
		[ -d "$card_backing/data/$card_path" ] && [ -d "$card_root/$card_path" ] ||
			card_fail "missing shared directory /$card_path"
	done <"$card_directories"
}

busybox mount -t devtmpfs -o nosuid,noexec,mode=0755 dev /dev || card_fail "cannot mount /dev"
busybox mount -t proc -o nosuid,nodev,noexec proc /proc || card_fail "cannot mount /proc"
busybox mount -t sysfs -o nosuid,nodev,noexec sysfs /sys || card_fail "cannot mount /sys"
busybox mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run || card_fail "cannot mount /run"
busybox mkdir -m 0700 "$card_backing" || card_fail "cannot create card mount point"

card_chosen=/sys/firmware/devicetree/base/chosen/fplinux,slot
if printf 'a\000' | busybox cmp -s "$card_chosen" -; then
	card_slot=a
elif printf 'b\000' | busybox cmp -s "$card_chosen" -; then
	card_slot=b
else
	card_fail "U-Boot did not supply a valid system slot"
fi
read -r card_build_id <"$card_identity/build-id" || card_fail "cannot read FIT build identity"
read -r card_uuid <"$card_identity/root-uuid" || card_fail "cannot read system partition UUID"
read -r card_wait <"$card_identity/wait-seconds" || card_fail "cannot read SD card wait time"
until find_card_partition; do
	[ "$card_wait" -gt 0 ] || card_fail "cannot find the SD system partition"
	busybox sleep 1
	card_wait=$((card_wait - 1))
done

# Admit the selected root before permitting ext4 journal replay or other writes.
busybox mount -t ext4 -o ro,noload "$card_partition" "$card_backing" ||
	card_fail "cannot inspect the SD system partition"
verify_card_root
busybox umount "$card_backing" || card_fail "cannot release the read-only SD mount"
busybox mount -t ext4 -o rw "$card_partition" "$card_backing" ||
	card_fail "cannot mount the writable SD system partition"
card_writable=1
verify_card_root

busybox mount --bind "$card_root" /newroot || card_fail "cannot mount the selected system root"
while read -r card_path _card_mode; do
	busybox mount --bind "$card_backing/data/$card_path" "/newroot/$card_path" ||
		card_fail "cannot mount shared directory /$card_path"
done <"$card_directories"
for card_mount in dev proc sys run; do
	busybox mount --move "/$card_mount" "/newroot/$card_mount" ||
		card_fail "cannot transfer /$card_mount"
done

# Moving /run transfers the writable ext4 mount along with its transient parent.
exec busybox switch_root -c /dev/console /newroot /sbin/init "$@"
