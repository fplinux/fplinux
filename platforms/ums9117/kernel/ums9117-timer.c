// SPDX-License-Identifier: GPL-2.0-only
/* Clocksource for the always-running 1 ms UMS9117 AP system counter. */
#include <linux/clocksource.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/sched_clock.h>

#define UMS9117_SYSCNT_CURRENT 0x0c
#define UMS9117_SYSCNT_HZ 1000

static void __iomem *ums9117_syscnt_base;

static u64 notrace ums9117_sched_clock_read(void)
{
	return readl_relaxed(ums9117_syscnt_base + UMS9117_SYSCNT_CURRENT);
}

static int __init ums9117_syscnt_init(struct device_node *node)
{
	void __iomem *base;
	int ret;

	base = of_iomap(node, 0);
	if (!base)
		return -ENXIO;

	ret = clocksource_mmio_init(base + UMS9117_SYSCNT_CURRENT,
				    "ums9117_syscnt", UMS9117_SYSCNT_HZ, 300,
				    32, clocksource_mmio_readl_up);
	if (ret)
		iounmap(base);
	else {
		ums9117_syscnt_base = base;
		sched_clock_register(ums9117_sched_clock_read, 32,
				     UMS9117_SYSCNT_HZ);
	}

	return ret;
}

TIMER_OF_DECLARE(ums9117_syscnt, "fplinux,ums9117-syscounter",
		 ums9117_syscnt_init);
