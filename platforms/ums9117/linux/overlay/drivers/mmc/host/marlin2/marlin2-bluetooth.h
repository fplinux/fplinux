/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_BLUETOOTH_H
#define FPLINUX_MARLIN2_BLUETOOTH_H

#include "marlin2-bluetooth-wire.h"

struct device;
struct marlin2_bluetooth;
struct marlin2_transport;

/*
 * Register only after the root filesystem and radio firmware are available.
 * HCI registration schedules controller setup immediately. pskey contains
 * admitted board configuration and the selected device address.
 */
struct marlin2_bluetooth *
marlin2_bluetooth_register(struct device *dev,
			   struct marlin2_transport *transport,
			   const u8 pskey[MARLIN2_BLUETOOTH_PSKEY_BYTES]);
/* Wait for initial HCI work; this does not switch controller power. */
int marlin2_bluetooth_wait_ready(struct marlin2_bluetooth *bt);
void marlin2_bluetooth_unregister(struct marlin2_bluetooth *bt);

#endif
