/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC27XX_TEST_KERNEL_H
#define FPLINUX_SC27XX_TEST_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;

#define BIT(n) (1U << (n))
#define GENMASK(high, low) ((~0U >> (31 - (high))) & (~0U << (low)))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define IS_ENABLED(option) (option)
#define THIS_MODULE NULL
#define REGULATOR_VOLTAGE 0
#define PROBE_PREFER_ASYNCHRONOUS 1
#define EPROBE_DEFER 517
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define of_match_ptr(pointer) (pointer)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_AUTHOR(author)
#define MODULE_LICENSE(license)
#define EXPORT_SYMBOL_GPL(symbol)
#define module_platform_driver(driver) \
	struct platform_driver *test_driver_##driver = &(driver)

struct regulator_dev;
struct regmap;

struct device {
	struct device *parent;
	const void *match_data;
	struct regmap *regmap;
	struct regulator_dev *resources[32];
	unsigned int num_resources;
};

struct platform_device {
	struct device dev;
};

struct of_device_id {
	const char *compatible;
	const void *data;
};

struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
		int probe_type;
	} driver;
};

struct regulator_ops {
	int (*enable)(struct regulator_dev *rdev);
	int (*disable)(struct regulator_dev *rdev);
	int (*is_enabled)(struct regulator_dev *rdev);
	int (*get_voltage)(struct regulator_dev *rdev);
	int (*list_voltage)(struct regulator_dev *rdev, unsigned int selector);
	int (*map_voltage)(struct regulator_dev *rdev, int min_uV, int max_uV);
	int (*get_voltage_sel)(struct regulator_dev *rdev);
	int (*set_voltage_sel)(struct regulator_dev *rdev,
			       unsigned int selector);
};

struct regulator_desc {
	const char *name;
	const char *of_match;
	bool of_match_full_name;
	const char *regulators_node;
	int id;
	const struct regulator_ops *ops;
	int type;
	void *owner;
	unsigned int n_voltages;
	int min_uV;
	int uV_step;
	int fixed_uV;
	unsigned int enable_time;
	u32 enable_reg;
	u32 enable_mask;
	unsigned int enable_val;
	bool enable_is_inverted;
	u32 vsel_reg;
	u32 vsel_mask;
};

struct regulator_config {
	struct device *dev;
	struct regmap *regmap;
};

struct regulator_dev {
	const struct regulator_desc *desc;
	struct regulator_config config;
};

const void *device_get_match_data(const struct device *dev);
struct regmap *dev_get_regmap(struct device *dev, const char *name);
int dev_err_probe(const struct device *dev, int error, const char *format, ...);
struct regulator_dev *
devm_regulator_register(struct device *dev, const struct regulator_desc *desc,
			const struct regulator_config *config);
struct regmap *rdev_get_regmap(struct regulator_dev *rdev);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int value);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value);
int regulator_enable_regmap(struct regulator_dev *rdev);
int regulator_disable_regmap(struct regulator_dev *rdev);
int regulator_is_enabled_regmap(struct regulator_dev *rdev);
int regulator_list_voltage_linear(struct regulator_dev *rdev,
				  unsigned int selector);
int regulator_map_voltage_linear(struct regulator_dev *rdev, int min_uV,
				 int max_uV);
int regulator_get_voltage_sel_regmap(struct regulator_dev *rdev);
int regulator_set_voltage_sel_regmap(struct regulator_dev *rdev,
				     unsigned int selector);

#endif
