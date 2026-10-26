/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_AUDIO_HOST_DEVICE_H
#define FPLINUX_QIN_AUDIO_HOST_DEVICE_H
struct device {
	int unused;
};
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void dev_info(struct device *dev, const char *format, ...);
void dev_err(struct device *dev, const char *format, ...);
#endif
