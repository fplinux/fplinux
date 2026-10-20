/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPRD_SPI_R4P0_CLOCK_H
#define FPLINUX_SPRD_SPI_R4P0_CLOCK_H

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

static inline int sprd_spi_r4p0_clock_params(u32 source_hz, u32 requested_hz,
					     u32 *divider, u32 *effective_hz)
{
	u32 prescale;

	if (!requested_hz || source_hz < 2 || requested_hz > source_hz / 2)
		return -EINVAL;

	/* Keep division consistent with the reported integer SCK rate. */
	prescale = DIV_ROUND_UP_ULL(source_hz / 2, requested_hz);
	*divider = prescale - 1;
	*effective_hz = source_hz / (2 * prescale);
	return 0;
}

#endif
