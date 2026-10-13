// SPDX-License-Identifier: GPL-2.0-only
/* Minimal DT machine descriptor for Unisoc UMS9117. */
#include <linux/bits.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/panic.h>

#include <asm/mach/arch.h>

#include "fplinux-platform-identity.h"

#define UMS9117_AON_APB_EB0 0x0000
#define UMS9117_AON_APB_RTC_EB 0x0010
#define UMS9117_AON_APB_EB2 0x00b0
#define UMS9117_AON_APB_CLK_EB0 0x0134
#define UMS9117_AON_APB_SET_OFFSET 0x1000
#define UMS9117_AON_APB_PIN_EB BIT(20)
#define UMS9117_AON_APB_PIN_APB_EB BIT(12)
#define UMS9117_TIMER_EB0_BITS (BIT(11) | BIT(10))
#define UMS9117_TIMER_RTC_BITS (BIT(4) | BIT(3))
#define UMS9117_TIMER_CLK_BITS BIT(11)

#define UMS9117_TIMER_CTL 0x10
#define UMS9117_TIMER_CTL_ENABLE BIT(1)
#define UMS9117_TIMER_INT 0x14
#define UMS9117_TIMER_INT_ENABLE BIT(0)
#define UMS9117_TIMER_INT_CLEAR BIT(3)

static void __iomem *__init ums9117_time_map_node(const char *compatible)
{
	struct device_node *node;
	void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		panic("UMS9117: missing %s timer resource", compatible);
	base = of_iomap(node, 0);
	of_node_put(node);
	if (!base)
		panic("UMS9117: cannot map %s timer resource", compatible);

	return base;
}

static void __init ums9117_enable_aon_gate(void __iomem *base, u32 reg,
					   u32 mask)
{
	writel(mask, base + reg + UMS9117_AON_APB_SET_OFFSET);
	if ((readl(base + reg) & mask) != mask)
		panic("UMS9117: AON gate did not enable at %#x", reg);
}

static void __init ums9117_init_time(void)
{
	void __iomem *aon_apb;
	void __iomem *timer;

	aon_apb = ums9117_time_map_node("sprd,ums9117-aon-apb");
	ums9117_enable_aon_gate(aon_apb, UMS9117_AON_APB_EB0,
				UMS9117_TIMER_EB0_BITS);
	ums9117_enable_aon_gate(aon_apb, UMS9117_AON_APB_RTC_EB,
				UMS9117_TIMER_RTC_BITS);
	ums9117_enable_aon_gate(aon_apb, UMS9117_AON_APB_CLK_EB0,
				UMS9117_TIMER_CLK_BITS);
	/* Pin registers must be accessible to later pinctrl consumers. */
	ums9117_enable_aon_gate(aon_apb, UMS9117_AON_APB_EB0,
				UMS9117_AON_APB_PIN_EB);
	ums9117_enable_aon_gate(aon_apb, UMS9117_AON_APB_EB2,
				UMS9117_AON_APB_PIN_APB_EB);
	iounmap(aon_apb);

	/* Stop and clear the clockevent before its driver requests the IRQ. */
	timer = ums9117_time_map_node("sprd,pike2-timer");
	writel(readl(timer + UMS9117_TIMER_CTL) & ~UMS9117_TIMER_CTL_ENABLE,
	       timer + UMS9117_TIMER_CTL);
	writel(UMS9117_TIMER_INT_CLEAR, timer + UMS9117_TIMER_INT);
	writel(0, timer + UMS9117_TIMER_INT);
	if ((readl(timer + UMS9117_TIMER_CTL) & UMS9117_TIMER_CTL_ENABLE) ||
	    (readl(timer + UMS9117_TIMER_INT) & UMS9117_TIMER_INT_ENABLE))
		panic("UMS9117: clockevent timer did not stop");
	iounmap(timer);

	/* The platform clock drivers probe after this early timer boundary. */
	of_clk_init(NULL);
	timer_probe();
	tick_setup_hrtimer_broadcast();
}

static const char *const ums9117_dt_compat[] __initconst = {
	FPLINUX_PLATFORM_COMPATIBLE,
	NULL,
};

DT_MACHINE_START(UMS9117_DT, FPLINUX_PLATFORM_DISPLAY_NAME).dt_compat =
	ums9117_dt_compat,
			     .init_time = ums9117_init_time, MACHINE_END
