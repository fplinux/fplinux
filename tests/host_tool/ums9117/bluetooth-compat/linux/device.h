/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_DEVICE_H
#define BLUETOOTH_HOST_DEVICE_H

struct device {
	int unused;
};

int devm_add_action_or_reset(struct device *device, void (*action)(void *),
			     void *data);

#define dev_err(device, format, ...) ((void)(device))

#endif
