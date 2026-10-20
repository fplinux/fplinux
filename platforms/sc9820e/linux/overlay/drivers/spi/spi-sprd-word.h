/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPRD_SPI_WORD_H
#define FPLINUX_SPRD_SPI_WORD_H

#include <linux/types.h>

#define SPRD_SPI_CTL0_WORD_LENGTH_MASK (0x1fU << 2)
#define SPRD_SPI_CTL0_WORD_LENGTH_SHIFT 2

static inline u32 sprd_spi_word_control(u32 control, unsigned int bits)
{
	/* The five-bit length field encodes a 32-bit word as zero. */
	return (control & ~SPRD_SPI_CTL0_WORD_LENGTH_MASK) |
	       ((bits << SPRD_SPI_CTL0_WORD_LENGTH_SHIFT) &
		SPRD_SPI_CTL0_WORD_LENGTH_MASK);
}

static inline u32 sprd_spi_word_count(u32 length, unsigned int bits)
{
	return length / (bits / 8);
}

#endif
