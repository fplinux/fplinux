/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GPU_CLOCK_HOST_H
#define FPLINUX_GPU_CLOCK_HOST_H

#include "sc27xx-test-kernel.h"

#define __iomem
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0x200
#define CLK_GET_RATE_NOCACHE BIT(6)
#define CLK_SET_RATE_GATE BIT(0)

struct clk {
	unsigned long rate;
	unsigned int enables;
	bool prepared;
	int error;
};
struct regulator {
	struct regulator_dev *rdev;
};
struct clk_hw;
struct clk_core;
struct clk_rate_request {
	struct clk_core *core;
	unsigned long rate;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long best_parent_rate;
	struct clk_hw *best_parent_hw;
};
struct clk_ops {
	int (*prepare)(struct clk_hw *hw);
	void (*unprepare)(struct clk_hw *hw);
	unsigned long (*recalc_rate)(struct clk_hw *hw,
				     unsigned long parent_rate);
	int (*determine_rate)(struct clk_hw *hw,
			      struct clk_rate_request *request);
	int (*set_rate)(struct clk_hw *hw, unsigned long rate,
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

unsigned long clk_get_rate(struct clk *clock);
int clk_enable(struct clk *clock);
void clk_disable(struct clk *clock);
int regulator_get_voltage(struct regulator *regulator);
u32 readl(const void *address);
void writel(u32 value, void *address);
void udelay(unsigned int microseconds);
void dev_err(const struct device *device, const char *format, ...);
void *devm_kzalloc(struct device *device, size_t size, unsigned int flags);
struct clk *devm_clk_get(struct device *device, const char *name);
struct regulator *devm_regulator_get(struct device *device, const char *name);
struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name);
struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name);
unsigned long resource_size(const struct resource *resource);
void *devm_ioremap_resource(struct device *device, struct resource *resource);
int devm_clk_hw_register(struct device *device, struct clk_hw *hw);
typedef struct clk_hw *(*host_clock_get)(void *args, void *data);
struct clk_hw *of_clk_hw_simple_get(void *args, void *data);
int devm_of_clk_add_hw_provider(struct device *device, host_clock_get get,
				void *data);
int device_property_read_u32(const struct device *device, const char *name,
			     u32 *value);

#endif
