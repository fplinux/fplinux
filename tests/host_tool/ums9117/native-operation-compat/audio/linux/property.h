/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_PROFILE_HOST_PROPERTY_H
#define FPLINUX_AUDIO_PROFILE_HOST_PROPERTY_H

#include <linux/types.h>

struct device;

bool device_property_present(struct device *dev, const char *name);
int device_property_read_string(struct device *dev, const char *name,
				const char **value);

#endif
