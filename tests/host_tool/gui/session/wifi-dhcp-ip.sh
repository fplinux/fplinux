#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

printf '%s\n' "$*" >>"${IP_TRACE:?}"
case "$*" in
"-4 route add "*)
	[ "${IP_REJECT_ROUTE:-}" != yes ]
	;;
esac
