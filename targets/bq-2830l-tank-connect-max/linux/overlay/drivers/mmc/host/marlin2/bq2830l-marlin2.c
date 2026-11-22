// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>

#include "bq2830l-marlin2.h"

static const struct marlin2_radio_pin radio_pins[] = {
	{ .offset = 0x054, .value = 0x30 },
	{ .offset = 0x454, .value = 0x82046 },
	{ .offset = 0x1b0, .value = 0 },
	{ .offset = 0x5b0, .value = 0x80001 },
	{ .offset = 0x058, .value = 0x30 },
	{ .offset = 0x458, .value = 0x82001 },
	{ .offset = 0x064, .value = 0x30 },
	{ .offset = 0x464, .value = 0x102001 },
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

	ret = gpiod_direction_output(reset, 1);
	if (ret)
		return ret;
	ret = gpiod_direction_output(enable, 0);
	if (ret)
		return ret;
	msleep(100);
	gpiod_set_value_cansleep(enable, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(reset, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(reset, 0);
	fsleep(2000);
	return 0;
}

const struct marlin2_radio_board bq2830l_marlin2 = {
	.pins = radio_pins,
	.num_pins = ARRAY_SIZE(radio_pins),
	.pins_phandle = "bq,pins",
	.enable_controls = enable_controls,
	.image_sha256 = {
		0x5f, 0xdd, 0x81, 0xda, 0x4e, 0x8e, 0xf9, 0xa5,
		0x20, 0xdc, 0xb1, 0xd6, 0x15, 0x43, 0xb0, 0xe6,
		0x2c, 0xb2, 0x58, 0xbe, 0x8b, 0x7a, 0x89, 0x66,
		0xff, 0xd6, 0x48, 0xbc, 0xcc, 0x3d, 0x56, 0x90,
	},
	.timeout_ms = 60000,
	.wcn_voltage_uv = 1587500,
	.fm = true,
};
