/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_PROFILE_HOST_FIRMWARE_H
#define FPLINUX_AUDIO_PROFILE_HOST_FIRMWARE_H

#include <linux/types.h>

struct device;
struct firmware {
	size_t size;
	const u8 *data;
};

int request_firmware(const struct firmware **firmware, const char *name,
		     struct device *dev);
void release_firmware(const struct firmware *firmware);

#endif
