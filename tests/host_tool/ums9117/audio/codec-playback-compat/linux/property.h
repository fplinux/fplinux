/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_PLAYBACK_PROPERTY_H
#define FPLINUX_CODEC_PLAYBACK_PROPERTY_H

#include <linux/types.h>

struct device;

bool device_property_read_bool(struct device *dev, const char *name);
int device_property_count_u32(struct device *dev, const char *name);
int device_property_read_u32_array(struct device *dev, const char *name,
				   u32 *values, size_t count);
int device_property_read_u32(struct device *dev, const char *name, u32 *value);

#endif
