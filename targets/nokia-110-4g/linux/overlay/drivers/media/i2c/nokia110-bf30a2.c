// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "bf30a2-internal.h"

static const struct bf30a2_reg nokia110_mode_regs[] = {
	{ 0xf2, 0x01 }, { 0xcf, 0xb0 }, { 0x12, 0x10 }, { 0x6b, 0x70 },
	{ 0x15, 0x10 }, { 0x00, 0x40 }, { 0xca, 0x03 }, { 0x1f, 0x01 },
	{ 0x1e, 0x26 }, { 0x8a, 0x33 }, { 0x04, 0x00 }, { 0x06, 0x26 },
	{ 0x08, 0x07 }, { 0x1c, 0x12 }, { 0x20, 0x20 }, { 0x21, 0x20 },
	{ 0x34, 0x02 }, { 0x35, 0x02 }, { 0x36, 0x21 }, { 0x37, 0x13 },
	{ 0xcb, 0x22 }, { 0xcc, 0x89 }, { 0xcd, 0x6c }, { 0xce, 0x6b },
	{ 0xa0, 0x8e }, { 0x01, 0x1b }, { 0x02, 0x1d }, { 0x13, 0x08 },
	{ 0x87, 0x13 }, { 0x8b, 0x08 }, { 0x70, 0x1f }, { 0x71, 0x43 },
	{ 0x72, 0x0a }, { 0x73, 0x68 }, { 0x74, 0x41 }, { 0x75, 0xa7 },
	{ 0x76, 0x12 }, { 0x77, 0xf8 }, { 0x78, 0x33 }, { 0x40, 0x32 },
	{ 0x41, 0x28 }, { 0x42, 0x26 }, { 0x43, 0x1d }, { 0x44, 0x1a },
	{ 0x45, 0x14 }, { 0x46, 0x11 }, { 0x47, 0x0f }, { 0x48, 0x0e },
	{ 0x49, 0x0d }, { 0x4b, 0x0c }, { 0x4c, 0x0b }, { 0x4e, 0x0a },
	{ 0x4f, 0x09 }, { 0x50, 0x09 }, { 0x24, 0x4d }, { 0x25, 0x36 },
	{ 0x80, 0x33 }, { 0x81, 0x20 }, { 0x82, 0x40 }, { 0x83, 0x50 },
	{ 0x84, 0x70 }, { 0x85, 0x50 }, { 0x86, 0xd8 }, { 0x89, 0x45 },
	{ 0x8f, 0x81 }, { 0x91, 0xff }, { 0x92, 0x08 }, { 0x94, 0x22 },
	{ 0x95, 0xfd }, { 0x9a, 0x30 }, { 0xf0, 0x4a }, { 0x51, 0x03 },
	{ 0x52, 0x22 }, { 0x53, 0x29 }, { 0x54, 0x0d }, { 0x57, 0x31 },
	{ 0x58, 0x03 }, { 0x59, 0x38 }, { 0x23, 0x33 }, { 0xa0, 0x8f },
	{ 0xa1, 0x93 }, { 0xa2, 0x0d }, { 0xa3, 0x25 }, { 0xa4, 0x07 },
	{ 0xa5, 0x25 }, { 0xa7, 0x80 }, { 0xa8, 0x80 }, { 0xa9, 0x24 },
	{ 0xaa, 0x22 }, { 0xab, 0x22 }, { 0xae, 0xa0 }, { 0xaf, 0x04 },
	{ 0xc8, 0x0f }, { 0xc9, 0x1b }, { 0xd3, 0x11 }, { 0xd4, 0x1d },
	{ 0xee, 0x0a }, { 0xef, 0x04 }, { 0x55, 0x34 }, { 0xb1, 0x98 },
	{ 0xb2, 0x98 }, { 0xb3, 0xd4 }, { 0xb4, 0x0c }, { 0x9e, 0xbc },
	{ 0x56, 0x98 }, { 0x13, 0x07 }, { 0x13, 0x07 }, { 0x13, 0x07 },
	{ 0x13, 0x07 }, { 0x13, 0x07 }, { 0x13, 0x07 }, { 0x13, 0x07 },
	{ 0x13, 0x07 },
};

static int get_board_gpios(struct device *dev, struct bf30a2_power *power)
{
	power->front_powerdown =
		devm_gpiod_get(dev, "front-powerdown", GPIOD_ASIS);
	if (IS_ERR(power->front_powerdown))
		return dev_err_probe(dev, PTR_ERR(power->front_powerdown),
				     "failed to get secondary power GPIO\n");
	return 0;
}

static int nokia110_power_off(struct bf30a2_power *power)
{
	int ret;
	int step_ret;

	ret = gpiod_direction_output(power->powerdown, 0);
	step_ret = gpiod_direction_output(power->front_powerdown, 0);
	if (!ret)
		ret = step_ret;
	clk_disable_unprepare(power->xclk);
	step_ret = regulator_disable(power->avdd);
	if (!ret)
		ret = step_ret;
	step_ret = regulator_disable(power->iovdd);
	if (!ret)
		ret = step_ret;
	return ret;
}

static int nokia110_power_on(struct bf30a2_power *power)
{
	int ret;

	ret = clk_set_rate(power->xclk, BF30A2_ID_XCLK_HZ);
	if (ret)
		return ret;
	ret = regulator_enable(power->avdd);
	if (ret)
		return ret;
	ret = regulator_enable(power->iovdd);
	if (ret)
		goto avdd_off;

	ret = gpiod_direction_output(power->front_powerdown, 1);
	if (ret)
		goto gpios_off;
	fsleep(2000);
	ret = gpiod_direction_output(power->powerdown, 0);
	if (ret)
		goto gpios_off;
	fsleep(2000);
	fsleep(1000);
	fsleep(2000);
	ret = gpiod_set_value_cansleep(power->powerdown, 0);
	if (ret)
		goto gpios_off;

	ret = clk_prepare_enable(power->xclk);
	if (ret)
		goto gpios_off;
	msleep(20);
	ret = gpiod_direction_output(power->reset, 0);
	if (ret)
		goto clock_off;
	msleep(20);
	ret = gpiod_set_value_cansleep(power->reset, 1);
	if (ret)
		goto clock_off;
	msleep(100);
	return 0;

clock_off:
	clk_disable_unprepare(power->xclk);
gpios_off:
	gpiod_direction_output(power->powerdown, 0);
	gpiod_direction_output(power->front_powerdown, 0);
	regulator_disable(power->avdd);
	regulator_disable(power->iovdd);
	return ret;
avdd_off:
	regulator_disable(power->avdd);
	return ret;
}

static const struct bf30a2_variant nokia110_bf30a2_variant = {
	.mode_regs = nokia110_mode_regs,
	.num_mode_regs = ARRAY_SIZE(nokia110_mode_regs),
	.avdd_uv = 2800000,
	.id_high = 0x3b,
	.id_low = 0x02,
	.reset_flags = GPIOD_ASIS,
	.powerdown_flags = GPIOD_ASIS,
	.stream_xclk_hz = BF30A2_MODE_XCLK_HZ,
	.get_board_gpios = get_board_gpios,
	.power_on = nokia110_power_on,
	.power_off = nokia110_power_off,
};

static const struct of_device_id nokia110_bf30a2_of_match[] = {
	{ .compatible = "nokia,110-4g-bf30a2",
	  .data = &nokia110_bf30a2_variant },
	{}
};
MODULE_DEVICE_TABLE(of, nokia110_bf30a2_of_match);

static struct i2c_driver nokia110_bf30a2_driver = {
	.driver = {
		.name = "nokia110-bf30a2",
		.of_match_table = nokia110_bf30a2_of_match,
		.pm = pm_ptr(&bf30a2_pm_ops),
	},
	.probe = bf30a2_probe,
	.remove = bf30a2_remove,
};
module_i2c_driver(nokia110_bf30a2_driver);

MODULE_DESCRIPTION("Nokia 110 4G BF30A2 camera sensor");
MODULE_LICENSE("GPL");
