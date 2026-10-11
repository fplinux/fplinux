/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_HOST_DELAY_H
#define FPLINUX_SPI_HOST_DELAY_H

void udelay(unsigned long delay);
void usleep_range(unsigned long minimum, unsigned long maximum);

#endif
