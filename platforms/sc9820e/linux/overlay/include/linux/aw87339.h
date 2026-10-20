/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AW87339_H
#define FPLINUX_AW87339_H

#include <linux/types.h>

struct aw87339;
struct device;

struct aw87339_profile {
	u8 initial_control;
	u8 registers_2_to_10[9];
	u8 active_control;
};

/* The managed consumer link keeps the supplier bound through consumer remove. */
struct aw87339 *devm_aw87339_get(struct device *consumer, const char *property);
int aw87339_start(struct aw87339 *amplifier,
		  const struct aw87339_profile *profile);
int aw87339_stop(struct aw87339 *amplifier);

#endif
