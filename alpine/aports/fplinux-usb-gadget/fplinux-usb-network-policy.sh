#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

verify_usb_forwarding() {
	usb_forwarding=$(cat "$1") || return 1
	[ "$usb_forwarding" = 0 ]
}

verify_usb_routes() {
	usb_routes=$("$1" route show) || return 1
	# BusyBox can ignore route filters, so inspect the complete table.
	printf '%s\n' "$usb_routes" | awk '
		$1 == "default" || $1 == "0.0.0.0/0" {
			for (field = 2; field < NF; field++)
				if ($field == "dev" && $(field + 1) == "usb0")
					exit 1
		}
	'
}
