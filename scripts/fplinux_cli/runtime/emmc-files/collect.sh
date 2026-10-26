#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

fail() {
	printf '%s\n' "eMMC file collection: $*" >&2
	exit 1
}

workspace=
mounted=0
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	if [ "$mounted" = 1 ]; then
		if umount "$workspace/mount"; then
			mounted=0
		else
			printf '%s\n' 'eMMC file collection: cannot unmount the selected partition' >&2
			[ "$status" -ne 0 ] || status=1
		fi
	fi
	if [ -n "$workspace" ] && [ "$mounted" = 0 ]; then
		if ! rm -rf -- "${workspace:?}"; then
			printf '%s\n' 'eMMC file collection: cannot remove the private RAM directory' >&2
			[ "$status" -ne 0 ] || status=1
		fi
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

find_partition() {
	partition=
	for entry in "$card_sys"/"$card"p*/uevent; do
		[ -f "$entry" ] || continue
		partition_label=$(sed -n 's/^PARTNAME=//p' "$entry") || fail 'cannot read partition names'
		[ "$partition_label" = "$1" ] || continue
		[ -z "$partition" ] || fail "partition name is repeated: $1"
		partition_sys=${entry%/uevent}
		partition=/dev/${partition_sys##*/}
	done
	[ -n "$partition" ] || fail "required partition is missing: $1"
	[ -b "$partition" ] && [ ! -L "$partition" ] || fail 'partition is not a block device'
	[ "$(cat "$partition_sys/ro")" = 1 ] || fail 'partition is not read-only'
}

find_source() {
	parent=$workspace/mount
	remainder=${1#/}
	while [ -n "$remainder" ]; do
		component=${remainder%%/*}
		current=$parent/$component
		# A successful directory listing distinguishes absence from a filesystem error.
		present=$(find "$parent" -mindepth 1 -maxdepth 1 -exec sh -c '
			requested=$1
			shift
			for candidate do
				if [ "$candidate" = "$requested" ]; then printf x; fi
			done
		' find "$current" {} +) || fail "cannot inspect source directory: $parent"
		[ -n "$present" ] || return 1
		[ ! -L "$current" ] || fail "source contains a symbolic link: $1"
		if [ "$component" = "$remainder" ]; then
			[ -f "$current" ] || fail "source is not a regular file: $1"
			source=$current
			return 0
		fi
		[ -d "$current" ] || fail "source ancestor is not a directory: $1"
		parent=$current
		remainder=${remainder#*/}
	done
	fail 'source path must name a file'
}

[ "$#" -ge 2 ] || fail 'incomplete request'
raw_device=$1
selection_count=$2
shift 2
case "$raw_device" in
/dev/mmcblk*)
	card=${raw_device#/dev/}
	card_number=${card#mmcblk}
	case "$card_number" in '' | *[!0-9]*) fail 'source must name a whole MMC card' ;; esac
	;;
*) fail 'source must name a whole MMC card' ;;
esac
case "$selection_count" in '' | *[!0-9]*) fail 'invalid selection count' ;; esac
[ "$selection_count" -gt 0 ] || fail 'empty selection'
[ -b "$raw_device" ] && [ ! -L "$raw_device" ] || fail 'source is not a block device'
card_sys=/sys/class/block/$card
[ "$(cat "$card_sys/removable")" = 0 ] || fail 'source is removable'
[ "$(cat "$card_sys/ro")" = 1 ] || fail 'source is not read-only'
[ "$(cat "$card_sys/device/type")" = MMC ] || fail 'source is not an MMC card'

umask 077
workspace=$(mktemp -d /run/fplinux-emmc.XXXXXX) || fail 'cannot create a private RAM directory'
mkdir "$workspace/mount" "$workspace/files" || fail 'cannot prepare the private RAM directory'
remaining=$selection_count
while [ "$remaining" -gt 0 ]; do
	[ "$#" -ge 3 ] || fail 'incomplete file selection'
	name=$1
	maximum=$2
	location_count=$3
	shift 3
	case "$name" in '' | . | .. | */* | *\\*) fail 'unsafe original name' ;; esac
	case "$maximum:$location_count" in *[!0-9:]*) fail 'invalid file limits' ;; esac
	[ "$maximum" -gt 0 ] && [ "$location_count" -gt 0 ] || fail 'invalid file limits'
	found=0
	while [ "$location_count" -gt 0 ]; do
		[ "$#" -ge 2 ] || fail 'incomplete source location'
		partition_name=$1
		path=$2
		shift 2
		location_count=$((location_count - 1))
		[ "$found" = 0 ] || continue
		case "$path" in /*) ;; *) fail 'source path must be absolute' ;; esac
		case "/${path#/}/" in *//* | */./* | */../*) fail 'unsafe source path' ;; esac
		find_partition "$partition_name"
		mount -t ext4 -o ro,noload "$partition" "$workspace/mount" || fail "cannot mount partition: $partition_name"
		mounted=1
		if find_source "$path"; then
			size=$(stat -c %s -- "$source") || fail "cannot stat source: $path"
			[ "$size" -gt 0 ] && [ "$size" -le "$maximum" ] || fail "source has an invalid size: $path"
			cp -- "$source" "$workspace/files/$name" || fail "cannot read source: $path"
			[ "$(stat -c %s -- "$workspace/files/$name")" = "$size" ] || fail "incomplete source: $path"
			found=1
		fi
		umount "$workspace/mount" || fail "cannot unmount partition: $partition_name"
		mounted=0
	done
	[ "$found" = 1 ] || fail "required original is missing: $name"
	remaining=$((remaining - 1))
done
[ "$#" -eq "$selection_count" ] || fail 'incomplete archive names'
tar -c -f - -C "$workspace/files" -- "$@" || fail 'cannot stream selected originals'
