/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/device.h>
int device_property_read_string(struct device *dev, const char *name,
				const char **value);
