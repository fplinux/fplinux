/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DRM_MIPI_DBI_SPI_H
#define FPLINUX_DRM_MIPI_DBI_SPI_H

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include <drm/drm_mipi_dbi.h>

static inline int mipi_dbi_spi_write_memory(struct spi_device *spi,
					    u32 speed_hz, u8 bpw,
					    const void *buffer, size_t length)
{
	const u16 *pixels = buffer;
	u32 *packed;
	size_t i;
	int ret;

	if (bpw != 16 || !length || length % 4 || spi->mode & SPI_LSB_FIRST ||
	    !spi_is_bpw_supported(spi, 32) || spi_max_transfer_size(spi) < 4)
		return mipi_dbi_spi_transfer(spi, speed_hz, bpw, buffer,
					     length);

	packed = kmalloc(length, GFP_KERNEL);
	if (!packed)
		return -ENOMEM;

	/* Preserve the two native 16-bit words in MSB-first wire order. */
	for (i = 0; i < length / 4; i++)
		packed[i] = (u32)get_unaligned(&pixels[2 * i]) << 16 |
			    get_unaligned(&pixels[2 * i + 1]);

	ret = mipi_dbi_spi_transfer(spi, speed_hz, 32, packed, length);
	kfree(packed);
	return ret;
}

#endif
