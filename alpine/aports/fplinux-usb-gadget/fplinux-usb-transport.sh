#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

read_usb_transport() {
	if [ ! -e "$1" ]; then
		printf 'ncm\n'
		return
	fi
	usb_transport_value=$(cat "$1") || return 1
	case "$usb_transport_value" in
	ncm | rndis) printf '%s\n' "$usb_transport_value" ;;
	*)
		printf 'unsupported USB transport: %s\n' "$usb_transport_value" >&2
		return 1
		;;
	esac
}

create_usb_network_function() {
	usb_transport_gadget=$1
	usb_transport_configuration=$2
	usb_transport=$3
	usb_transport_function=$usb_transport_gadget/functions/$usb_transport.usb0
	mkdir "$usb_transport_function"
	printf '%s\n' "$4" >"$usb_transport_function/dev_addr"
	printf '%s\n' "$5" >"$usb_transport_function/host_addr"
	printf 'usb%%d\n' >"$usb_transport_function/ifname"
	case "$usb_transport" in
	ncm)
		printf 'Keyboard and NCM\n' \
			>"$usb_transport_configuration/strings/0x409/configuration"
		;;
	rndis)
		printf 'ef\n' >"$usb_transport_function/class"
		printf '04\n' >"$usb_transport_function/subclass"
		printf '01\n' >"$usb_transport_function/protocol"
		printf 'RNDIS and keyboard\n' \
			>"$usb_transport_configuration/strings/0x409/configuration"
		printf '1\n' >"$usb_transport_gadget/os_desc/use"
		printf '0xcd\n' >"$usb_transport_gadget/os_desc/b_vendor_code"
		printf 'MSFT100\n' >"$usb_transport_gadget/os_desc/qw_sign"
		printf 'RNDIS\n' \
			>"$usb_transport_function/os_desc/interface.rndis/compatible_id"
		printf '\000\000\000\000\000\000\000\000' \
			>"$usb_transport_function/os_desc/interface.rndis/sub_compatible_id"
		ln -s "$usb_transport_configuration" "$usb_transport_gadget/os_desc/c.1"
		;;
	esac
}

link_usb_functions() {
	usb_transport_gadget=$1
	usb_transport_configuration=$2
	usb_transport=$3
	if [ "$usb_transport" = rndis ]; then
		ln -s "$usb_transport_gadget/functions/rndis.usb0" \
			"$usb_transport_configuration/rndis.usb0"
	fi
	ln -s "$usb_transport_gadget/functions/gser.keyboard" \
		"$usb_transport_configuration/gser.keyboard"
	if [ "$usb_transport" = ncm ]; then
		ln -s "$usb_transport_gadget/functions/ncm.usb0" \
			"$usb_transport_configuration/ncm.usb0"
	fi
}
