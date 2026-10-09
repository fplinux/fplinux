#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# shellcheck source=../../../alpine/aports/fplinux-usb-gadget/fplinux-usb-transport.sh
. "$1"
transport=$(read_usb_transport "$2/usb-transport")
create_usb_network_function "$2/gadget" "$2/gadget/configs/c.1" "$transport" \
	02:00:00:00:00:01 02:00:00:00:00:02
link_usb_functions "$2/gadget" "$2/gadget/configs/c.1" "$transport"
printf '%s\n' "$transport"
