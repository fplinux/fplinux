#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# Regular directories replace ConfigFS. Only automatic children and function
# ordering are modeled; no kernel descriptors, interfaces or USB are exercised.
case "${0##*/}" in
mkdir)
	/bin/mkdir "$@"
	case "$1" in
	*/functions/rndis.usb0) /bin/mkdir -p "$1/os_desc/interface.rndis" ;;
	esac
	;;
ln)
	/bin/ln "$@"
	case "$3" in
	*/configs/c.1/*)
		printf '%s\n' "${3##*/}" >>"${3%/*}/.function-order"
		;;
	esac
	;;
*) exit 2 ;;
esac
