/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BF30A2_INTERNAL_H
#define FPLINUX_BF30A2_INTERNAL_H

#include <linux/gpio/consumer.h>
#include <linux/types.h>

#define BF30A2_ID_XCLK_HZ 12000000UL
#define BF30A2_MODE_XCLK_HZ 24000000UL

struct clk;
struct dev_pm_ops;
struct device;
struct i2c_client;
struct regulator;

struct bf30a2_reg {
	u16 address;
	u8 value;
};

struct bf30a2_power {
	struct clk *xclk;
	struct regulator *core;
	struct regulator *avdd;
	struct regulator *iovdd;
	struct gpio_desc *reset;
	struct gpio_desc *powerdown;
	struct gpio_desc *front_powerdown;
};

struct bf30a2_variant {
	const struct bf30a2_reg *mode_regs;
	size_t num_mode_regs;
	int avdd_uv;
	bool requires_core;
	u8 id_high;
	u8 id_low;
	enum gpiod_flags reset_flags;
	enum gpiod_flags powerdown_flags;
	unsigned long stream_xclk_hz;
	int (*get_board_gpios)(struct device *dev, struct bf30a2_power *power);
	int (*power_on)(struct bf30a2_power *power);
	int (*power_off)(struct bf30a2_power *power);
};

int bf30a2_probe(struct i2c_client *client);
void bf30a2_remove(struct i2c_client *client);
extern const struct dev_pm_ops bf30a2_pm_ops;

#endif
