#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

fail() {
	printf 'card update: %s\n' "$*" >&2
	exit 1
}

offline_apk() {
	apk --no-network --no-cache --repositories-file /dev/null "$@"
}

candidate_apk() {
	offline_apk --root "$candidate" --keys-dir "$restore_keys" --repository "$index" "$@"
}

remove_restore_keys() {
	rm -f -- "${restore_keys:?}"/*.pub
	rmdir -- "$restore_keys"
}

require_selected_packages() {
	solution=$1
	while IFS=' ' read -r coordinate selection; do
		[ -n "$coordinate" ] || continue
		if ! printf '%s\n' "$solution" | grep -Fxq -- "$coordinate"; then
			fail "$selection: APK selection requires $coordinate from the new release"
		fi
	done <<EOF
$expected
EOF
}

[ "$#" = 3 ] || fail "usage: card-apk-world OLD_SLOT CANDIDATE_ROOT APK_DIRECTORY"
old_slot=$1
candidate=$2
apk_directory=$3

added=$(awk '
	FILENAME == ARGV[1] { base[$0] = 1; next }
	NF && !($0 in base) { print }
' "$old_slot/base-world" "$old_slot/root/etc/apk/world") || fail "cannot read the active APK world"
[ -n "$added" ] || exit 0

index=$apk_directory/Packages.adb
release_key=$apk_directory/fplinux-build.rsa.pub
[ -f "$index" ] && [ -f "$release_key" ] || fail "missing the release APK index or signing key"

# Trust the admitted build key for this transaction alongside the base roots.
restore_keys=$(mktemp -d /tmp/fplinux-apk-keys.XXXXXXXX)
trap remove_restore_keys EXIT
trap 'exit 1' HUP INT TERM
for key in "$candidate/lib/apk/keys/"*.pub "$candidate/etc/apk/keys/"*.pub; do
	[ -f "$key" ] || continue
	cp -- "$key" "$restore_keys/"
done
cp -- "$release_key" "$restore_keys/"
offline_apk --root "$candidate" --keys-dir "$restore_keys" verify "$index" ||
	fail "cannot verify the release APK index"

# A local APK filename becomes an identity pin in world. The old digest must
# still identify the installed package, and the new release supplies its build.
expected=
set --
while IFS= read -r selection; do
	package=${selection#!}
	package=${package%%[<>=~@]*}
	case "$package" in
	'' | *[!a-zA-Z0-9._+:\-]*)
		fail "$selection: invalid APK selection"
		;;
	esac
	case "$selection" in
	*'@'* | '!'*'><'*)
		fail "$selection: unsupported APK constraint for an offline release"
		;;
	'!'*)
		set -- "$@" "$selection"
		continue
		;;
	*'><'*)
		installed=$(offline_apk --root "$old_slot/root" info --exists -- "$selection") &&
			[ "$installed" = "$package" ] ||
			fail "$package: local APK pin does not identify the active installed package"
		selection=$package
		;;
	esac

	coordinate=$(candidate_apk query --from none --match name --summarize package -- "$package") &&
		[ -n "$coordinate" ] || fail "$package: no APK in the new release"
	expected="$expected$coordinate $selection
"
	set -- "$@" "$selection"
done <<EOF
$added
EOF

# A virtual provider can satisfy the solver while leaving the selected APK
# absent. Check the real package versions both in the plan and after install.
planned=$(candidate_apk query --world --summarize package -- "$@") ||
	fail "cannot resolve APK selection: $*"
require_selected_packages "$planned"

candidate_apk add -- "$@" || fail "cannot restore APK selection: $*"
installed=$(offline_apk --root "$candidate" query --installed --summarize package -- '*') ||
	fail "cannot read restored APK selections"
require_selected_packages "$installed"
