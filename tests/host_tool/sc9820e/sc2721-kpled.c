// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kpled-compat/kpled-test-kernel.h"

extern struct platform_driver *test_driver_sc2721_kpled_driver;

/* Substitute only the regmap transport and LED/devres subsystem boundaries. */
struct regmap {
	unsigned int words[0x1000 / 4];
	unsigned int fail_reg;
	unsigned int fail_mask;
	int error;
	bool write_before_error;
	unsigned int enabled_word;
};

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	(void)flags;
	assert(!dev->allocation);
	dev->allocation = calloc(1, size);
	return dev->allocation;
}

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
	(void)name;
	return dev->regmap;
}

int device_property_read_u32(struct device *dev, const char *name, u32 *value)
{
	(void)dev;
	assert(!strcmp(name, "reg"));
	*value = 0xeac;
	return 0;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data)
{
	assert(!dev->cleanup);
	dev->cleanup = action;
	dev->cleanup_data = data;
	return 0;
}

int devm_led_classdev_register_ext(struct device *dev, struct led_classdev *led,
				   struct led_init_data *init)
{
	assert(init->fwnode == dev);
	if (dev->fail_registration)
		return -ENOSPC;
	dev->led = led;
	return 0;
}

void led_classdev_suspend(struct led_classdev *led)
{
	assert(led->flags & LED_CORE_SUSPENDRESUME);
	assert(led->brightness_set_blocking(led, LED_OFF) == 0);
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	unsigned int previous;
	int error = 0;

	assert(reg < sizeof(map->words) && reg % 4 == 0);
	if (reg == map->fail_reg && mask == map->fail_mask) {
		error = map->error;
		map->error = 0;
	}
	if (error && !map->write_before_error)
		return error;
	previous = map->words[reg / 4];
	map->words[reg / 4] = (previous & ~mask) | (value & mask);
	if (reg == 0xeb0 && (previous & 0x8000) &&
	    !(map->words[reg / 4] & 0x8000))
		map->enabled_word = map->words[reg / 4];
	return error;
}

static void release_device(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	if (dev->led) {
		assert(dev->led->brightness_set_blocking(dev->led, LED_OFF) ==
		       0);
		dev->led = NULL;
	}
	if (dev->cleanup)
		dev->cleanup(dev->cleanup_data);
	free(dev->allocation);
	memset(dev, 0, sizeof(*dev));
}

static void test_brightness(void)
{
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct led_classdev *led;

	map.words[0xeac / 4] = 0xa15a;
	map.words[0xeb0 / 4] = 0x1275;
	assert(test_driver_sc2721_kpled_driver->probe(&pdev) == 0);
	led = pdev.dev.led;
	assert(led && led->max_brightness == 255);
	assert(map.words[0xeac / 4] == 0xa55a);
	assert(map.words[0xeb0 / 4] == 0x9275);
	assert(led->brightness_set_blocking(led, 1) == 0);
	assert(map.enabled_word == 0x00f5);
	assert(map.words[0xeb0 / 4] == 0x00f5);
	assert(led->brightness_set_blocking(led, 128) == 0);
	assert(map.words[0xeb0 / 4] == 0x4075);
	assert(led->brightness_set_blocking(led, 255) == 0);
	assert(map.words[0xeb0 / 4] == 0x7ff5);
	assert(led->brightness_set_blocking(led, LED_OFF) == 0);
	assert(map.words[0xeb0 / 4] == 0xfff5);
	assert(map.words[0xeac / 4] == 0xa55a);
	release_device(&pdev);
}

static void test_errors(void)
{
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct led_classdev *led;

	map.words[0xeb0 / 4] = 0x0035;
	assert(test_driver_sc2721_kpled_driver->probe(&pdev) == 0);
	led = pdev.dev.led;
	map.fail_reg = 0xeb0;
	map.fail_mask = 0x7f80;
	map.error = -EIO;
	map.write_before_error = true;
	assert(led->brightness_set_blocking(led, 64) == -EIO);
	assert(map.words[0xeb0 / 4] == 0xa035);
	map.fail_mask = 0x8000;
	map.error = -ETIMEDOUT;
	assert(led->brightness_set_blocking(led, 255) == -ETIMEDOUT);
	assert(map.words[0xeb0 / 4] == 0xffb5);
	assert(led->brightness_set_blocking(led, 1) == 0);
	map.fail_reg = 0xeac;
	map.fail_mask = 0x400;
	map.error = -EIO;
	map.write_before_error = false;
	assert(led->brightness_set_blocking(led, LED_OFF) == -EIO);
	assert(map.words[0xeb0 / 4] == 0x80b5);
	release_device(&pdev);
}

static void test_lifecycle(void)
{
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct led_classdev *led;

	assert(test_driver_sc2721_kpled_driver->probe(&pdev) == 0);
	led = pdev.dev.led;
	assert(led->brightness_set_blocking(led, 128) == 0);
	test_driver_sc2721_kpled_driver->shutdown(&pdev);
	assert(map.words[0xeb0 / 4] == 0xc000);
	assert(led->brightness_set_blocking(led, 64) == 0);
	release_device(&pdev);
	assert(map.words[0xeb0 / 4] == 0xa000);
	pdev.dev.parent = &parent;
	pdev.dev.fail_registration = true;
	assert(test_driver_sc2721_kpled_driver->probe(&pdev) == -ENOSPC);
	assert(!pdev.dev.led);
	assert(map.words[0xeb0 / 4] == 0xa000);
	release_device(&pdev);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "brightness"))
		test_brightness();
	else if (!strcmp(argv[1], "errors"))
		test_errors();
	else if (!strcmp(argv[1], "lifecycle"))
		test_lifecycle();
	else
		return 2;
	puts("SC2721 keypad LED callbacks passed");
	return 0;
}
