// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>

#include "itel-neo-r60-marlin2.h"

static const struct marlin2_radio_pin radio_pins[] = {
	{ .offset = 0x054, .value = 0x30 },
	{ .offset = 0x454, .value = 0x82046 },
	{ .offset = 0x1b0, .value = 0 },
	{ .offset = 0x5b0, .value = 0x80001 },
	{ .offset = 0x058, .value = 0x30 },
	{ .offset = 0x458, .value = 0x82001 },
	{ .offset = 0x064, .value = 0x30 },
	{ .offset = 0x464, .value_property = "fplinux,radio-pad-0464" },
	{ .offset = 0x1c4, .value = 0 },
	{ .offset = 0x1c8, .value = 0 },
	{ .offset = 0x1cc, .value = 0 },
	{ .offset = 0x1d0, .value = 0 },
	{ .offset = 0x1d4, .value = 0 },
	{ .offset = 0x1d8, .value = 0 },
	{ .offset = 0x5c4, .value = 0x102004 },
	{ .offset = 0x5c8, .value = 0x102088 },
	{ .offset = 0x5cc, .value = 0x102088 },
	{ .offset = 0x5d0, .value = 0x102088 },
	{ .offset = 0x5d4, .value = 0x102088 },
	{ .offset = 0x5d8, .value = 0x102088 },
};

static int enable_controls(struct gpio_desc *enable, struct gpio_desc *reset)
{
	int ret;

	ret = gpiod_direction_output(enable, 0);
	if (ret)
		return ret;
	msleep(100);
	gpiod_set_value_cansleep(enable, 1);
	ret = gpiod_direction_output(reset, 1);
	if (ret) {
		gpiod_set_value_cansleep(enable, 0);
		return ret;
	}
	fsleep(2000);
	gpiod_set_value_cansleep(reset, 0);
	fsleep(2000);
	msleep(20);
	return 0;
}

const struct marlin2_radio_board itel_neo_r60_marlin2 = {
	.pins = radio_pins,
	.num_pins = ARRAY_SIZE(radio_pins),
	.pins_phandle = "fplinux,radio-pins",
	.enable_controls = enable_controls,
	.image_sha256 = {
		0x45, 0x3f, 0x52, 0xe2, 0x04, 0xaa, 0x12, 0x11,
		0xc9, 0x9e, 0xf8, 0x1e, 0xc6, 0x65, 0x34, 0x0c,
		0xff, 0xff, 0x91, 0x7a, 0x0d, 0xa7, 0xea, 0x84,
		0xf4, 0x6a, 0x18, 0xf6, 0xd8, 0xb0, 0x26, 0x1e,
	},
	/* Verify the complete image at 400 kHz on the one-bit SDIO bus. */
	.timeout_ms = 120000,
	.wcn_voltage_uv = 1587500,
	.wifipa_first = true,
	.require_get_info = true,
};
