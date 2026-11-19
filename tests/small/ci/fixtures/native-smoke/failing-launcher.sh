#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
if [ "$#" -eq 0 ]; then
	cat menu.txt
	read -r choice
	[ "$choice" = 0 ]
	exit
fi

printf 'fixture launcher cannot initialize\n' >&2
exit 23
