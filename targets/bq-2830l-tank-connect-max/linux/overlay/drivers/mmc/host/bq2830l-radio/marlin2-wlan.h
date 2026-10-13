/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_WLAN_H
#define FPLINUX_MARLIN2_WLAN_H

#include <linux/types.h>

struct device;
struct marlin2_transport;
struct marlin2_wlan;

/*
 * The board supplies the admitted device address before the WLAN channel
 * opens. Reading it does not acquire or release the shared radio transport.
 */
struct marlin2_wlan_radio_ops {
	int (*get_mac)(void *context, u8 mac[6]);
};

struct marlin2_wlan *
marlin2_wlan_register(struct device *dev,
		      const struct marlin2_wlan_radio_ops *radio_ops,
		      void *radio_context, struct marlin2_transport *transport);
void marlin2_wlan_unregister(struct marlin2_wlan *wlan);

#endif
