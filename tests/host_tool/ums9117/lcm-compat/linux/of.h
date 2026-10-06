/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/types.h>
struct device_node;
int of_property_read_u32_array(const struct device_node *node, const char *name,
			       u32 *values, size_t count);
