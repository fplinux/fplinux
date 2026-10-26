#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

sudo apt-get update
sudo apt-get install --no-install-recommends -y uidmap zstd udev iproute2

fplinux_account=$(id -un)
for fplinux_kind in subuid subgid; do
	if ! awk -F: -v account="$fplinux_account" '$1 == account && $3 >= 65536 { found = 1 } END { exit !found }' "/etc/$fplinux_kind"; then
		fplinux_first=$(awk -F: 'BEGIN { last = 100000 } $2 + $3 > last { last = $2 + $3 } END { print last }' "/etc/$fplinux_kind")
		fplinux_last=$((fplinux_first + 65535))
		case "$fplinux_kind" in
		subuid) sudo usermod --add-subuids "$fplinux_first-$fplinux_last" "$fplinux_account" ;;
		subgid) sudo usermod --add-subgids "$fplinux_first-$fplinux_last" "$fplinux_account" ;;
		esac
	fi
done

if [ -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
	sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
fi

./fplinux ci bootstrap
