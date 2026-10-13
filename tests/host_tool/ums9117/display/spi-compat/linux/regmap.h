/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_HOST_REGMAP_H
#define FPLINUX_SPI_HOST_REGMAP_H

#include <linux/types.h>

struct regmap;
int regmap_write(struct regmap *map, unsigned int reg, unsigned int value);

#endif
