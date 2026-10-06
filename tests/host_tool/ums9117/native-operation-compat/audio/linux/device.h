/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_PROFILE_HOST_DEVICE_H
#define FPLINUX_AUDIO_PROFILE_HOST_DEVICE_H

struct device;

int dev_err_probe(struct device *dev, int error, const char *format, ...);

#endif
