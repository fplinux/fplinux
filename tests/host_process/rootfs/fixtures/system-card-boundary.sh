#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

export PATH=/usr/bin:/bin
tool=${0##*/}
case "$tool" in
awk | cat)
	for argument; do
		shift
		case "$argument" in
		/proc/self/mountinfo) argument=${FPLINUX_CARD_FILES:?}/mountinfo ;;
		/proc/partitions) argument=${FPLINUX_CARD_FILES:?}/partitions ;;
		/proc/swaps) argument=${FPLINUX_CARD_FILES:?}/swaps ;;
		/sys/dev/block/179:10/uevent) argument=${FPLINUX_CARD_FILES:?}/uevent ;;
		/sys/class/block/mmcblk1/device/type) argument=${FPLINUX_CARD_FILES:?}/type ;;
		/sys/firmware/devicetree/base/compatible) argument=${FPLINUX_CARD_FILES:?}/compatible ;;
		esac
		set -- "$@" "$argument"
	done
	exec "$tool" "$@"
	;;
sync)
	printf 'sync:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
	;;
rc-service)
	printf 'rc-service:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
	exit "${FPLINUX_STOP_STATUS:?}"
	;;
poweroff)
	printf 'poweroff:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
	;;
umount)
	printf 'umount:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
	[ "${FPLINUX_UMOUNT_STATUS:?}" -eq 0 ] || exit "$FPLINUX_UMOUNT_STATUS"
	awk -v target="$1" '$5 != target' "${FPLINUX_CARD_FILES:?}/mountinfo" >"$FPLINUX_CARD_FILES/unmounted" &&
		mv "$FPLINUX_CARD_FILES/unmounted" "$FPLINUX_CARD_FILES/mountinfo"
	;;
mount)
	printf 'mount:%s\n' "$*" >>"${FPLINUX_GROW_CALLS:?}"
	[ "${FPLINUX_MOUNT_STATUS:?}" -eq 0 ] || exit "$FPLINUX_MOUNT_STATUS"
	awk -v target="$4" -v super_readonly="${FPLINUX_SUPER_READONLY:?}" '
		$5 == target {
			$6 = "ro,nosuid,nodev"
			if (super_readonly == "true") {
				$NF = "ro"
			}
		}
		{ print }
	' "${FPLINUX_CARD_FILES:?}/mountinfo" >"$FPLINUX_CARD_FILES/remounted" &&
		mv "$FPLINUX_CARD_FILES/remounted" "$FPLINUX_CARD_FILES/mountinfo"
	;;
esac
