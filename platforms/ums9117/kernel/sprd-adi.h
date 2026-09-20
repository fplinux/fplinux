/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPRD_ADI_H
#define FPLINUX_SPRD_ADI_H

struct spi_device;

/* Final SC2720 power-off: no sleeping SPI queue after device shutdown. */
int sprd_adi_sc2720_power_off(struct spi_device *spi);

#endif
