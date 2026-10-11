/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_HOST_PLATFORM_DEVICE_H
#define FPLINUX_SPI_HOST_PLATFORM_DEVICE_H

#include "../../lcm-compat/linux/platform_device.h"

void *devm_ioremap_resource(struct device *dev, struct resource *resource);

#endif
