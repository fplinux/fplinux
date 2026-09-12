/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_FIRMWARE_H
#define BLUETOOTH_HOST_FIRMWARE_H

#include <linux/types.h>

/* The fixture supplies synthetic records instead of the kernel loader. */
struct firmware {
	size_t size;
	const u8 *data;
};

struct device;

int request_firmware_direct(const struct firmware **fw, const char *name,
			    struct device *device);
void release_firmware(const struct firmware *fw);

#endif
