/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_IO_H
#define LCM_HOST_IO_H

#include <linux/types.h>

#define __iomem
typedef u64 phys_addr_t;
typedef u64 dma_addr_t;
typedef u64 resource_size_t;

struct device;
void *devm_ioremap(struct device *dev, resource_size_t start,
		   resource_size_t size);
u32 readl(const void *address);
void writel(u32 value, void *address);
void writew(u16 value, void *address);

#endif
