/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_HOST_BITS_H
#define FPLINUX_CODEC_HOST_BITS_H

#define BIT(bit) (1UL << (bit))
#define GENMASK(high, low)   \
	(((~0UL) << (low)) & \
	 ((~0UL) >> (sizeof(unsigned long) * 8 - 1 - (high))))

#endif
