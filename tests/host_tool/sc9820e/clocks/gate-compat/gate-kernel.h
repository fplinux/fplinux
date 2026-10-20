/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_GATE_HOST_KERNEL_H
#define FPLINUX_SC9820E_GATE_HOST_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define BIT(bit) (1UL << (bit))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define struct_size(pointer, member, count) \
	(sizeof(*(pointer)) + sizeof((pointer)->member[0]) * (count))
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0x200
#define CLK_SET_RATE_NO_REPARENT BIT(0)
#define CLK_GET_RATE_NOCACHE BIT(1)
#define __iomem
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)

struct regmap;
struct device_node {
	struct regmap *regmap;
	struct regmap *ap_ahb_regmap;
};
struct device {
	struct device_node *of_node;
	const void *match_data;
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
	const void *data;
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};
struct clk_hw;
struct clk_ops {
	int (*enable)(struct clk_hw *hw);
	void (*disable)(struct clk_hw *hw);
	int (*is_enabled)(struct clk_hw *hw);
};
struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
};
struct clk_hw {
	const struct clk_init_data *init;
};
struct clk_parent_data {
	const char *fw_name;
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

const void *device_get_match_data(const struct device *dev);
void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags);
struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name);
int regmap_write(struct regmap *regmap, unsigned int offset,
		 unsigned int value);
int regmap_read(struct regmap *regmap, unsigned int offset,
		unsigned int *value);
int devm_clk_hw_register(struct device *dev, struct clk_hw *hw);
int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data);
struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *args, void *data);
struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data);
struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index);
unsigned long resource_size(const struct resource *resource);
void *devm_platform_ioremap_resource(struct platform_device *pdev,
				     unsigned int index);
struct clk_hw *devm_clk_hw_register_mux_parent_data_table(
	struct device *dev, const char *name,
	const struct clk_parent_data *parents, u8 num_parents,
	unsigned long flags, void *reg, u8 shift, u8 width, u8 mux_flags,
	const u32 *table, void *lock);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void dev_err(struct device *dev, const char *format, ...);

#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define EXPORT_SYMBOL_GPL(symbol)
/* Registration exposes the real probe without changing production linkage. */
#define module_platform_driver(driver) \
	struct platform_driver *host_##driver = &(driver)

extern struct platform_driver *host_sc9820e_peripheral_gates_driver;
extern struct platform_driver *host_sc9820e_sdio_clock_driver;

#endif
