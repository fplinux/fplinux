/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_FM_H
#define FPLINUX_MARLIN2_FM_H

#include <linux/types.h>

struct device;
struct marlin2_fm;
struct marlin2_transport;

/*
 * The owner serializes device registration and teardown. Unregister drains
 * the channel callbacks before releasing component state.
 */
struct marlin2_fm *marlin2_fm_register(struct device *dev,
				       struct marlin2_transport *transport);
void marlin2_fm_unregister(struct marlin2_fm *radio);

#endif
