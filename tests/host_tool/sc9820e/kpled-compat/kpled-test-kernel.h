/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_KPLED_TEST_KERNEL_H
#define FPLINUX_KPLED_TEST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;

#define BIT(bit) (1U << (bit))
#define GENMASK(high, low) ((~0U >> (31 - (high))) & (~0U << (low)))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define LED_CORE_SUSPENDRESUME 1
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define module_platform_driver(driver) \
	struct platform_driver *test_driver_##driver = &(driver)

struct regmap;
struct led_classdev;

struct device {
	struct device *parent;
	struct regmap *regmap;
	void *allocation;
	void (*cleanup)(void *data);
	void *cleanup_data;
	struct led_classdev *led;
	bool fail_registration;
};

struct platform_device {
	struct device dev;
	void *data;
};

struct of_device_id {
	const char *compatible;
};

struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	void (*shutdown)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};

enum led_brightness {
	LED_OFF = 0,
	LED_FULL = 255,
};

struct led_classdev {
	unsigned int max_brightness;
	unsigned int flags;
	int (*brightness_set_blocking)(struct led_classdev *led,
				       enum led_brightness brightness);
};

struct led_init_data {
	void *fwnode;
};

/* This boundary checks serial callback use, not real kernel concurrency. */
struct mutex {
	bool held;
};

static inline void mutex_init(struct mutex *mutex)
{
	mutex->held = false;
}

static inline void mutex_lock(struct mutex *mutex)
{
	assert(!mutex->held);
	mutex->held = true;
}

static inline void mutex_unlock(struct mutex *mutex)
{
	assert(mutex->held);
	mutex->held = false;
}

static inline void *dev_fwnode(struct device *dev)
{
	return dev;
}

static inline void platform_set_drvdata(struct platform_device *pdev,
					void *data)
{
	pdev->data = data;
}

static inline void *platform_get_drvdata(struct platform_device *pdev)
{
	return pdev->data;
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags);
struct regmap *dev_get_regmap(struct device *dev, const char *name);
int device_property_read_u32(struct device *dev, const char *name, u32 *value);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void dev_err(struct device *dev, const char *format, ...);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data);
int devm_led_classdev_register_ext(struct device *dev, struct led_classdev *led,
				   struct led_init_data *init);
void led_classdev_suspend(struct led_classdev *led);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value);

#endif
