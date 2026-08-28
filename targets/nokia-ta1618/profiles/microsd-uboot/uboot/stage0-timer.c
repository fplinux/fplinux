// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <errno.h>
#include <timer.h>

#include <fplinux-boot-layout.h>

#include "stage0-handoff.h"

static uint64_t stage0_timer_get_count(struct udevice *dev)
{
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();

	return timer_conv_64(ops->timer_ms());
}

static int stage0_timer_probe(struct udevice *dev)
{
	return fplinux_stage0_ops() ? 0 : -ENODEV;
}

static const struct timer_ops stage0_timer_ops = {
	.get_count = stage0_timer_get_count,
};

uint64_t timer_early_get_count(void)
{
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();

	return ops ? ops->timer_ms() : 0;
}

unsigned long timer_early_get_rate(void)
{
	return FPLINUX_BOOT_LAYOUT_TIMER_HZ;
}

static const struct udevice_id stage0_timer_ids[] = {
	{ .compatible = "fplinux,stage0-timer" },
	{}
};

U_BOOT_DRIVER(stage0_timer) = {
	.name = "stage0_timer",
	.id = UCLASS_TIMER,
	.of_match = stage0_timer_ids,
	.probe = stage0_timer_probe,
	.ops = &stage0_timer_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
