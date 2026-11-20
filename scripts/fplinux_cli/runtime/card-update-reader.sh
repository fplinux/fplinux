#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu
umask 077

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

real_file() {
	[ -f "$1" ] && [ ! -L "$1" ]
}

real_directory() {
	[ -d "$1" ] && [ ! -L "$1" ]
}

decimal() {
	case "$1" in
	0) ;;
	'' | 0* | *[!0-9]*) return 1 ;;
	esac
	[ "${#1}" -le 12 ]
}

block_number() {
	[ -b "$1" ] || return 1
	device_hex=$(busybox stat -Lc '%t:%T' "$1") || return 1
	printf '%d:%d\n' "0x${device_hex%:*}" "0x${device_hex#*:}"
}

check_unused() {
	# Filesystem device numbers also catch aliases and bind mounts of ext4.
	busybox awk -v disk="$disk_number" -v boot="$boot_number" -v root="$root_number" '
		$3 == disk || $3 == boot || $3 == root { busy = 1 }
		END { exit busy }
	' /proc/self/mountinfo || fail "Unmount the SD card and release its swap before continuing"
	while read -r source type _; do
		[ "$type" = partition ] || continue
		swap=$(block_number "$(printf '%b' "$source")") || continue
		case "$swap" in
		"$disk_number" | "$boot_number" | "$root_number")
			fail "Unmount the SD card and release its swap before continuing"
			;;
		esac
	done </proc/swaps
}

check_mbr() {
	mbr=$(busybox od -An -v -j 440 -N 72 -tu1 "$device") || fail "Cannot read the SD card partition table"
	printf '%s\n' "$mbr" | busybox awk \
		-v signature="$disk_signature" -v boot_start="$boot_start" -v boot_size="$boot_size" \
		-v root_start="$root_start" -v root_size="$root_size" '
		function u32(at) {
			return byte[at] + 256 * byte[at + 1] + 65536 * byte[at + 2] + 16777216 * byte[at + 3]
		}
		{ for (i = 1; i <= NF; i++) byte[++count] = $i }
		END {
			if (count != 72 || byte[71] != 85 || byte[72] != 170 || u32(1) != signature ||
			    byte[7] != 0 || byte[11] != 12 || u32(15) != boot_start || u32(19) != boot_size ||
			    byte[23] != 0 || byte[27] != 131 || u32(31) != root_start || u32(35) != root_size)
				exit 1
			for (i = 39; i <= 70; i++) if (byte[i] != 0) exit 1
		}
	' || fail "The SD card MBR does not match the system card layout"
}

check_card() {
	[ -b "$device" ] && [ "$(busybox cat "$slot/device/type")" = SD ] ||
		fail "The selected SD card is no longer present"
	[ "$(busybox cat "$slot/device/cid")" = "$cid" ] || fail "The SD card changed"
	[ "$(busybox cat "$slot/ro")" = 0 ] && [ "$(busybox cat "$root_slot/ro")" = 0 ] ||
		fail "The selected SD card is read-only"
	[ "$(busybox cat "$slot/queue/logical_block_size")" = 512 ] ||
		fail "The card must use 512-byte sectors"
	[ -b "$boot_partition" ] && [ -b "$root_partition" ] || fail "The SD card has no system partitions"
	for partition in "$slot"/"${device##*/}"p*; do
		case "$partition" in
		"$boot_slot" | "$root_slot") ;;
		*) fail "The SD card has unexpected partitions" ;;
		esac
	done
	root_size=$(busybox cat "$root_slot/size")
	disk_size=$(busybox cat "$slot/size")
	decimal "$root_size" && decimal "$disk_size" &&
		[ "$root_size" -ge "$minimum_root_size" ] && [ "$((root_start + root_size))" -le "$disk_size" ] &&
		[ "$(busybox cat "$boot_slot/start")" = "$boot_start" ] &&
		[ "$(busybox cat "$boot_slot/size")" = "$boot_size" ] &&
		[ "$(busybox cat "$root_slot/start")" = "$root_start" ] ||
		fail "The SD card partition geometry does not match the system card layout"
	disk_number=$(busybox cat "$slot/dev")
	boot_number=$(busybox cat "$boot_slot/dev")
	root_number=$(busybox cat "$root_slot/dev")
	[ "$(block_number "$device")" = "$disk_number" ] &&
		[ "$(block_number "$boot_partition")" = "$boot_number" ] &&
		[ "$(block_number "$root_partition")" = "$root_number" ] ||
		fail "The SD card device nodes changed"
	check_unused
	check_mbr
}

mount_card() {
	busybox mount -t ext4 -o "$1,nosuid,nodev" "$root_partition" "$mount_dir" ||
		fail "Cannot mount the system card ext4 partition"
	mounted=1
}

unmount_card() {
	unmount_status=0
	if [ "$writable" -eq 1 ] && ! busybox sync -f "$mount_dir"; then
		printf '%s\n' 'Cannot sync the system card; keep the phone powered' >&2
		unmount_status=1
	fi
	if busybox umount "$mount_dir"; then
		mounted=0
	else
		printf '%s\n' 'Cannot unmount the system card; keep the phone powered' >&2
		unmount_status=1
	fi
	return "$unmount_status"
}

check_system() {
	real_directory "$system" && real_file "$system/target" &&
		printf '%s\n' "$target" | busybox cmp -s "$system/target" - ||
		fail "The SD card is not a stable FPLinux system card for this target"
	real_file "$system/active" && [ "$(busybox wc -c <"$system/active")" -eq 2 ] &&
		IFS= read -r active <"$system/active" || fail "The system card active slot is invalid"
	case "$active" in
	a | b) ;;
	*) fail "The system card active slot is invalid" ;;
	esac
}

free_bytes() {
	statistics=$(busybox stat -f -c '%a %S' "$mount_dir") || fail "Cannot check system card space"
	# shellcheck disable=SC2086 # stat reports two decimal integers separated by a space.
	set -- $statistics
	[ "$#" -eq 2 ] && decimal "$1" && decimal "$2" || fail "Cannot check system card space"
	printf '%s\n' "$(($1 * $2))"
}

check_staging() {
	if [ -e "$staging" ] || [ -L "$staging" ]; then
		real_directory "$staging" || fail "The system card update directory is invalid"
	fi
}

cleanup() {
	status=$?
	trap - EXIT
	trap '' HUP INT TERM
	if [ "$mounted" -eq 1 ] && ! unmount_card; then
		[ "$status" -ne 0 ] || status=1
	fi
	busybox rm -f -- "$work/card-update-reader.sh" "$work/card-slots.sh" \
		"$work/update-card.sh" "$work/card-apk-world.sh" "$work/transfer.sha256" || :
	busybox rmdir -- "$mount_dir" "$work" 2>/dev/null || :
	if [ "$status" -eq 0 ]; then
		printf '%s\n' "$outcome"
	fi
	exit "$status"
}

case "$0" in
/tmp/fplinux-card-update.*/card-update-reader.sh) work=${0%/*} ;;
*) fail "Invalid system card reader work directory" ;;
esac
mount_dir=$work/mount
system=$mount_dir/system
staging=$system/.update
mounted=0
writable=0
outcome=
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ "$#" -eq 10 ] || [ "$#" -eq 15 ] || fail "Invalid system card reader arguments"
action=$1 device=$2 cid=$3 target=$4 build=$5
disk_signature=$6 boot_start=$7 boot_size=$8 root_start=$9 minimum_root_size=${10}
case "$device" in
/dev/mmcblk[0-9] | /dev/mmcblk[0-9][0-9]) ;;
*) fail "Expected a whole SD card device" ;;
esac
case "$cid" in
'' | *[!0-9a-f]*) fail "Invalid SD card CID" ;;
esac
[ "${#cid}" -eq 32 ] || fail "Invalid SD card CID"
case "$target" in
'' | [!a-z0-9]* | *[!a-z0-9-]*) fail "Invalid system card target" ;;
esac
case "$build" in
'' | *[!0-9a-f]*) fail "Invalid system card build identity" ;;
esac
[ "${#build}" -eq 12 ] || fail "Invalid system card build identity"
for number in "$disk_signature" "$boot_start" "$boot_size" "$root_start" "$minimum_root_size"; do
	decimal "$number" || fail "Invalid system card layout"
done
[ "$disk_signature" -le 4294967295 ] && [ "$boot_start" -gt 0 ] && [ "$boot_size" -gt 0 ] &&
	[ "$((boot_start + boot_size))" -eq "$root_start" ] && [ "$minimum_root_size" -gt 0 ] ||
	fail "Invalid system card layout"
case "$action:$#" in
update:15)
	required=${11} fit_hash=${12} root_hash=${13} board_hash=${14} checksums_hash=${15}
	decimal "$required" && [ "$required" -gt 0 ] || fail "Invalid system card space requirement"
	;;
rollback:10) ;;
*) fail "Unknown system card reader action" ;;
esac

[ -f /etc/fplinux/preparation ] && awk '
	$5 == "/" { for (i = 7; i <= NF; i++) if ($i == "-" && $(i + 1) == "overlay") ram = 1 }
	END { exit !ram }
' /proc/self/mountinfo || fail "Boot the FPLinux RAM reader before using the system card"

slot=/sys/block/${device##*/}
boot_partition=${device}p1
root_partition=${device}p2
boot_slot=$slot/${boot_partition##*/}
root_slot=$slot/${root_partition##*/}

# Installation and updates must never own the card concurrently, even across SSH clients.
exec 9>/run/fplinux-card-install.lock
busybox flock -n 9 || fail "Another system card operation is still active; keep the phone powered"
check_card
busybox mkdir "$mount_dir"
mount_card ro,noload,noexec
check_system

if [ "$action" = update ]; then
	slot_state=$(sh "$work/update-card.sh" inspect "$mount_dir" "$target" "$build" "$fit_hash")
	case "$slot_state" in
	'active a' | 'active b')
		outcome="active $build"
		exit 0
		;;
	'inactive a' | 'inactive b') ;;
	*) fail "Cannot identify an inactive system card slot" ;;
	esac
	check_staging
	available=$(free_bytes)
	if [ -d "$staging" ]; then
		blocks=$(busybox du -sk "$staging") || fail "Cannot check existing update staging size"
		blocks=${blocks%%[[:space:]]*}
		decimal "$blocks" || fail "Cannot check existing update staging size"
		available=$((available + blocks * 1024))
	fi
	[ "$available" -ge "$required" ] || fail "The system card does not have enough free space for the update"
fi

unmount_card
check_card
# APK lifecycle hooks run inside the verified candidate root during preparation.
writable=1
mount_card rw
check_system

if [ "$action" = rollback ]; then
	sh "$work/card-slots.sh" "$mount_dir" "$target" rollback "$build" >&2
	outcome="rolled-back $build"
	exit 0
fi

if [ "$(sh "$work/update-card.sh" inspect "$mount_dir" "$target" "$build" "$fit_hash")" != "$slot_state" ]; then
	fail "The system card selection changed during read-only admission; retry the update"
fi
check_staging
busybox rm -rf -- "${staging:?}"
[ "$(free_bytes)" -ge "$required" ] || fail "The system card does not have enough free space for the update"
busybox mkdir -p "$staging/apks"
busybox tar -xf - -C "$staging" || fail "The system card update transfer was interrupted"
busybox sync -f "$staging" || fail "Cannot sync the transferred system card files"
(
	cd "$staging"
	busybox sha256sum -cs "$work/transfer.sha256"
) || fail "The transferred system card files failed verification"
sh "$work/update-card.sh" prepare "$mount_dir" "$target" "$build" "$fit_hash" "$root_hash" "$board_hash" "$checksums_hash" >&2
sh "$work/update-card.sh" publish "$mount_dir" "$target" "$build" "$fit_hash" "$checksums_hash" >&2
busybox rm -rf -- "${staging:?}"
outcome="updated $build"
