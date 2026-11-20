#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Manage system pairs on a mounted FPLinux card.

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

valid_build_id() {
	case "$1" in
	'' | *[!0-9a-f]*) return 1 ;;
	esac
	[ "${#1}" -eq 12 ]
}

pair_build_id() {
	pair=$1
	real_directory "$pair" && real_file "$pair/FPLINUX.ITB" &&
		[ -s "$pair/FPLINUX.ITB" ] && real_directory "$pair/root" &&
		real_directory "$pair/root/etc" && real_file "$pair/root/etc/os-release" ||
		fail "$2 lacks its FIT or root"
	build_id=$(busybox awk -F= '
		$1 == "BUILD_ID" { id = substr($0, 10); count++ }
		END { if (count != 1) exit 1; print id }
	' "$pair/root/etc/os-release") && valid_build_id "$build_id" ||
		fail "$2 lacks a build identity"
	printf '%s\n' "$build_id"
}

slot_build_id() {
	pair_build_id "$system/$1" "System card slot $1"
}

active_slot() {
	real_file "$system/active" && [ "$(busybox wc -c <"$system/active")" -eq 2 ] &&
		IFS= read -r selected <"$system/active" ||
		fail "The system card active slot is invalid"
	case "$selected" in
	a | b) printf '%s\n' "$selected" ;;
	*) fail "The system card active slot is invalid" ;;
	esac
}

matching_record() {
	real_file "$1" && printf '%s\n' "$2" | busybox cmp -s "$1" -
}

ready_record() {
	digest=$(busybox sha256sum "$system/$1/FPLINUX.ITB") || return 1
	printf '%s %s\n' "$2" "${digest%% *}"
}

confirmed_build() (
	build_id=$(slot_build_id "$1") && [ "$build_id" = "$2" ] &&
		record=$(ready_record "$1" "$build_id") && matching_record "$system/$1/ready" "$record"
)

pending=

cleanup() {
	if [ -n "$pending" ]; then
		rm -f -- "${pending:?}"
	fi
}

flush_card() {
	busybox sync -f "$system" || fail "Cannot flush the system card"
}

publish() {
	# The caller owns the card lock and has completed staging before publication.
	if [ -e "$1" ] || [ -L "$1" ]; then
		real_file "$1" || fail "The system card record is invalid: ${1##*/}"
	fi
	pending=$(busybox mktemp "$system/.record.XXXXXXXX")
	printf '%s\n' "$2" >"$pending"
	flush_card
	busybox mv -f "$pending" "$1" || fail "Cannot publish the system card record: ${1##*/}"
	pending=
	flush_card
}

[ "$#" -eq 4 ] || { [ "$#" -eq 5 ] && [ "$3" = activate ]; } ||
	fail "Invalid system card record arguments"
system=$1/system
target=$2
action=$3
requested=$4
prepared=${5-}
case "$target" in
'' | [!a-z0-9]* | *[!a-z0-9-]*) fail "Invalid system card target" ;;
esac
valid_build_id "$requested" || fail "Invalid system card build identity"
real_directory "$system" || fail "The selected card has no FPLinux system directory"

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "$action" = initialize ]; then
	[ "$(slot_build_id a)" = "$requested" ] || fail "The initial slot belongs to another build"
	if [ -e "$system/target" ] || [ -L "$system/target" ]; then
		matching_record "$system/target" "$target" || fail "The system card belongs to another target"
	else
		[ ! -e "$system/active" ] && [ ! -L "$system/active" ] ||
			fail "The system card already has an active slot"
		publish "$system/target" "$target"
	fi
	if [ -e "$system/active" ] || [ -L "$system/active" ]; then
		[ "$(active_slot)" = a ] || fail "The system card already has an active slot"
		flush_card
	else
		publish "$system/active" a
	fi
	exit 0
fi

matching_record "$system/target" "$target" || fail "The system card belongs to another target"
active=$(active_slot) || exit 1
case "$active" in
a) other=b ;;
b) other=a ;;
esac

case "$action" in
plan-update)
	current=$(slot_build_id "$active") || exit 1
	if [ "$current" = "$requested" ]; then
		printf 'active %s\n' "$active"
	else
		confirmed_build "$active" "$current" ||
			fail "Confirm that the active system boots before updating again"
		printf 'inactive %s\n' "$other"
	fi
	;;
rollback)
	if confirmed_build "$other" "$requested" 2>/dev/null; then
		publish "$system/active" "$other"
	elif confirmed_build "$active" "$requested" 2>/dev/null; then
		flush_card
	else
		fail "The requested system has no intact confirmed boot"
	fi
	;;
confirm)
	current=$(slot_build_id "$active") || exit 1
	[ "$current" = "$requested" ] || fail "The running build is not active on this system card"
	record=$(ready_record "$active" "$current") || fail "Cannot verify the active FIT"
	ready=$system/$active/ready
	if [ -e "$ready" ] || [ -L "$ready" ]; then
		matching_record "$ready" "$record" || fail "The active pair differs from its confirmed boot"
		flush_card
	else
		publish "$ready" "$record"
	fi
	;;
activate)
	current=$(slot_build_id "$active") || exit 1
	if [ "$current" = "$requested" ]; then
		flush_card
	else
		confirmed_build "$active" "$current" ||
			fail "Confirm that the active system boots before updating again"
		if [ -n "$prepared" ]; then
			[ "$(pair_build_id "$prepared" "The prepared system")" = "$requested" ] ||
				fail "The prepared pair belongs to another build"
			busybox rm -rf -- "$system/$other" || fail "Cannot remove the previous inactive slot"
			busybox mv -T "$prepared" "$system/$other" || fail "Cannot install the prepared system pair"
		else
			[ "$(slot_build_id "$other")" = "$requested" ] || fail "The other slot belongs to another build"
		fi
		publish "$system/active" "$other"
	fi
	;;
*) fail "Unknown system card record action" ;;
esac
