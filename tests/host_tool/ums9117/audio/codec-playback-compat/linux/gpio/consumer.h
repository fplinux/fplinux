/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_PLAYBACK_GPIO_H
#define FPLINUX_CODEC_PLAYBACK_GPIO_H

#include <linux/types.h>

struct device;
struct gpio_desc;

enum gpiod_flags { GPIOD_ASIS };

struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name,
				 enum gpiod_flags flags);
bool gpiod_cansleep(struct gpio_desc *gpio);
int gpiod_direction_output(struct gpio_desc *gpio, int value);
int gpiod_set_value(struct gpio_desc *gpio, int value);
int gpiod_set_value_cansleep(struct gpio_desc *gpio, int value);

#endif
