#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

[ ! -e /initial-root-only ]
printf 'system-init\n'
for argument; do
	printf 'argument=%s\n' "$argument"
done
printf 'transport='
if [ -f /etc/fplinux/usb-transport ]; then
	/libexec/busybox cat /etc/fplinux/usb-transport
else
	printf 'absent\n'
fi
printf 'preparation='
if [ -f /etc/fplinux/preparation ]; then
	/libexec/busybox cat /etc/fplinux/preparation
else
	printf 'absent\n'
fi
