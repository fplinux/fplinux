#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

card_access_dir=/etc/fplinux/card-access

autonomous_card_boot() {
	boot_entry=/proc/device-tree/chosen/fplinux,boot-entry
	[ -f "$boot_entry" ] && [ "$(tr -d '\000' <"$boot_entry")" = sd ]
}

card_access_provisioned() {
	[ -d "$card_access_dir" ] && [ ! -L "$card_access_dir" ] || return 1
	[ "$(stat -c '%a:%u:%g' "$card_access_dir")" = 700:0:0 ] || return 1
	for card_access_file in device-id usb-network client-key host-key; do
		card_access_path=$card_access_dir/$card_access_file
		[ -f "$card_access_path" ] && [ ! -L "$card_access_path" ] || return 1
		[ "$(stat -c '%a:%u:%g' "$card_access_path")" = 600:0:0 ] || return 1
	done
}

offline_card_boot() {
	autonomous_card_boot && ! card_access_provisioned
}

read_usb_transport() {
	usb_transport_value=$2
	if [ -z "$usb_transport_value" ]; then
		if [ ! -e "$1" ]; then
			printf 'ncm\n'
			return
		fi
		usb_transport_value=$(cat "$1") || return 1
	fi
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
		usb_description=NCM
		[ "$6" -eq 0 ] || usb_description="Keyboard and NCM"
		;;
	rndis)
		usb_description=RNDIS
		[ "$6" -eq 0 ] || usb_description="RNDIS and keyboard"
		printf 'ef\n' >"$usb_transport_function/class"
		printf '04\n' >"$usb_transport_function/subclass"
		printf '01\n' >"$usb_transport_function/protocol"
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
	printf '%s\n' "$usb_description" >"$usb_transport_configuration/strings/0x409/configuration"
}

link_usb_functions() {
	usb_transport_gadget=$1
	usb_transport_configuration=$2
	usb_transport=$3
	if [ "$usb_transport" = rndis ]; then
		ln -s "$usb_transport_gadget/functions/rndis.usb0" \
			"$usb_transport_configuration/rndis.usb0"
	fi
	if [ "$4" -eq 1 ]; then
		ln -s "$usb_transport_gadget/functions/gser.keyboard" \
			"$usb_transport_configuration/gser.keyboard"
	fi
	if [ "$usb_transport" = ncm ]; then
		ln -s "$usb_transport_gadget/functions/ncm.usb0" \
			"$usb_transport_configuration/ncm.usb0"
	fi
}
