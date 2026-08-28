// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <errno.h>
#include <serial.h>

#include "stage0-handoff.h"

static int stage0_console_setbrg(struct udevice *dev, int baudrate)
{
	return 0;
}

static int stage0_console_putc(struct udevice *dev, const char byte)
{
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();

	if (!ops)
		return -ENODEV;
	return ops->console_putc((uint8_t)byte) < 0 ? -EIO : 0;
}

static int stage0_console_probe(struct udevice *dev)
{
	if (!fplinux_stage0_ops())
		return -ENODEV;
	return 0;
}

static const struct dm_serial_ops stage0_console_ops = {
	.putc = stage0_console_putc,
	.setbrg = stage0_console_setbrg,
};

static const struct udevice_id stage0_console_ids[] = {
	{ .compatible = "fplinux,stage0-console" },
	{}
};

U_BOOT_DRIVER(stage0_console) = {
	.name = "stage0_console",
	.id = UCLASS_SERIAL,
	.of_match = stage0_console_ids,
	.probe = stage0_console_probe,
	.ops = &stage0_console_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
