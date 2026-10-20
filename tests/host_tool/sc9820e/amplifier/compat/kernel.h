/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AW87339_HOST_KERNEL_H
#define FPLINUX_AW87339_HOST_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define BIT(bit) (1UL << (bit))
#define GENMASK(high, low) \
	((UINT32_MAX >> (31 - (high))) & (UINT32_MAX << (low)))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define struct_size(pointer, member, count) \
	(sizeof(*(pointer)) + sizeof((pointer)->member[0]) * (count))
#define GFP_KERNEL 0
#define GPIOD_OUT_HIGH 1
#define I2C_FUNC_SMBUS_BYTE_DATA 0x180000
#define IORESOURCE_MEM 0x200
#define CLK_GET_RATE_NOCACHE BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER BIT(1)
#define __iomem
#define EPROBE_DEFER 517
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)

struct regmap;
struct i2c_client;
struct device_driver;
struct device_node {
	struct regmap *regmap;
};
struct device {
	struct device_node *of_node;
	struct device_driver *driver;
	void *data;
	unsigned int references;
	bool bound;
	bool locked;
};
struct device_link {
	struct device *consumer;
	struct device *supplier;
	u32 flags;
};
struct mutex {
	bool locked;
};
struct resource {
	unsigned long start;
	unsigned long end;
};
struct platform_device {
	struct device dev;
	struct resource *resource;
};
struct of_device_id {
	const char *compatible;
};
struct dev_pm_ops {
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
};
struct device_driver {
	const char *name;
	const struct of_device_id *of_match_table;
	const struct dev_pm_ops *pm;
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	struct device_driver driver;
};
struct gpio_desc {
	int physical_level;
};
struct i2c_adapter {
	bool byte_data;
};
struct i2c_client {
	struct device dev;
	struct i2c_adapter *adapter;
};
struct i2c_driver {
	int (*probe)(struct i2c_client *client);
	void (*shutdown)(struct i2c_client *client);
	struct device_driver driver;
};
struct clk_hw;
struct clk_ops {
	int (*enable)(struct clk_hw *hw);
	void (*disable)(struct clk_hw *hw);
	int (*is_enabled)(struct clk_hw *hw);
	unsigned long (*recalc_rate)(struct clk_hw *hw,
				     unsigned long parent_rate);
};
struct clk_parent_data {
	const char *fw_name;
};
struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
	const struct clk_parent_data *parent_data;
	unsigned int num_parents;
	unsigned long flags;
};
struct clk_hw {
	const struct clk_init_data *init;
};
struct clk_hw_onecell_data {
	unsigned int num;
	struct clk_hw *hws[];
};
struct of_phandle_args {
	unsigned int args_count;
	unsigned int args[1];
};
typedef struct clk_hw *(*host_clk_get)(struct of_phandle_args *args,
				       void *data);

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void dev_err(struct device *dev, const char *format, ...);
void dev_info(struct device *dev, const char *format, ...);
void *dev_get_drvdata(struct device *dev);
void device_lock(struct device *dev);
void device_unlock(struct device *dev);
bool device_is_bound(struct device *dev);
void put_device(struct device *dev);
struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags);
struct device_node *of_parse_phandle(struct device_node *node,
				     const char *property, int index);
void of_node_put(struct device_node *node);
struct i2c_client *of_find_i2c_device_by_node(struct device_node *node);
void mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
void i2c_set_clientdata(struct i2c_client *client, void *data);
void *i2c_get_clientdata(struct i2c_client *client);
bool i2c_check_functionality(struct i2c_adapter *adapter, unsigned long mask);
struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name,
				 int flags);
int gpiod_set_value_cansleep(struct gpio_desc *descriptor, int value);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data);
int i2c_smbus_read_byte_data(struct i2c_client *client, unsigned int reg);
int i2c_smbus_write_byte_data(struct i2c_client *client, unsigned int reg,
			      unsigned int value);
void msleep(unsigned int milliseconds);
struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index);
unsigned long resource_size(const struct resource *resource);
void *devm_platform_ioremap_resource(struct platform_device *pdev,
				     unsigned int index);
u32 readl(const void *address);
struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name);
int regmap_write(struct regmap *regmap, unsigned int offset,
		 unsigned int value);
int regmap_read(struct regmap *regmap, unsigned int offset,
		unsigned int *value);
int devm_clk_hw_register(struct device *dev, struct clk_hw *hw);
int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data);
struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data);

#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_function, resume_function) \
	const struct dev_pm_ops name = {                                  \
		.suspend = (suspend_function),                            \
		.resume = (resume_function),                              \
	}
#define pm_sleep_ptr(pointer) (pointer)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define EXPORT_SYMBOL_GPL(symbol)
#define module_i2c_driver(driver) struct i2c_driver *host_##driver = &(driver)
#define module_platform_driver(driver) \
	struct platform_driver *host_##driver = &(driver)

extern struct i2c_driver *host_aw87339_driver;
extern struct platform_driver *host_sc9820e_i2c2_clock_driver;

#endif
