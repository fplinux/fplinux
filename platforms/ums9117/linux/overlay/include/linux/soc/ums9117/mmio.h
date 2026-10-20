/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_MMIO_H
#define UMS9117_MMIO_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/platform_device.h>

static inline void __iomem *
ums9117_devm_ioremap_shared(struct platform_device *pdev, const char *name,
			    resource_size_t minimum_size)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < minimum_size)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static inline void __iomem *
ums9117_devm_ioremap_shared_register(struct platform_device *pdev,
				     const char *name, resource_size_t address)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource->start != address ||
	    resource_size(resource) != 4)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

#endif
