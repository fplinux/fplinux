/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_HOST_DELAY_H
#define FPLINUX_CODEC_HOST_DELAY_H

void usleep_range(unsigned long minimum, unsigned long maximum);
void msleep(unsigned int milliseconds);

#endif
