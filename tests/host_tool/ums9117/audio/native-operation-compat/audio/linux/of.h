/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_PROFILE_HOST_OF_H
#define FPLINUX_AUDIO_PROFILE_HOST_OF_H

struct device_node;
extern struct device_node *of_root;

int of_property_read_string_index(const struct device_node *node,
				  const char *name, int index,
				  const char **value);

#endif
