/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_PIXEL_PEER_KERNEL_H
#define FPLINUX_SPI_PIXEL_PEER_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <asm-generic/errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define GFP_KERNEL 0
#define SPI_LSB_FIRST 8

struct spi_device {
	unsigned int mode;
	bool supports_32;
	size_t max_transfer;
};

void *kmalloc(size_t length, int flags);
void kfree(const void *buffer);
int mipi_dbi_spi_transfer(struct spi_device *spi, u32 speed_hz, u8 bpw,
			  const void *buffer, size_t length);

static inline bool spi_is_bpw_supported(struct spi_device *spi,
					unsigned int bpw)
{
	return bpw == 32 && spi->supports_32;
}

static inline size_t spi_max_transfer_size(struct spi_device *spi)
{
	return spi->max_transfer;
}

static inline u16 get_unaligned(const u16 *pointer)
{
	u16 value;

	memcpy(&value, pointer, sizeof(value));
	return value;
}

#endif
