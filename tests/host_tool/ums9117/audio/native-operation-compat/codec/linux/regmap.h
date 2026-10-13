/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_HOST_REGMAP_H
#define FPLINUX_CODEC_HOST_REGMAP_H

struct regmap;
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int value);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value);
int regmap_write_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		      unsigned int value);

#endif
