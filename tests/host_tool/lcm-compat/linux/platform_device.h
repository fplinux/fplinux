/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_PLATFORM_DEVICE_H
#define LCM_HOST_PLATFORM_DEVICE_H

#include <linux/io.h>
#include <linux/kernel.h>

/* Init is compiled but discarded by the linker; these APIs are not faked. */
struct device_node;
struct device {
	struct device_node *of_node;
};
struct platform_device {
	struct device dev;
};
struct resource {
	phys_addr_t start;
};
#define IORESOURCE_MEM 0x00000200
void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name);
struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name);
int dev_err_probe(struct device *dev, int error, const char *format, ...);

#endif
