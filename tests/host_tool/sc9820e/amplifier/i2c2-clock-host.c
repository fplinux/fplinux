// SPDX-License-Identifier: GPL-2.0-only
#include "compat/kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct regmap {
	unsigned int value;
	unsigned int writes;
	unsigned int offset;
	unsigned int written;
	int error;
};

struct fixture {
	u32 source_register;
	struct regmap ap_apb;
	struct clk_hw *registered[2];
	struct clk_init_data init[2];
	unsigned int registrations;
	struct clk_hw_onecell_data *clocks;
	void *allocations[4];
	size_t allocation_count;
};

static struct fixture *active;

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	void *allocation = calloc(1, size);

	(void)dev;
	(void)flags;
	assert(allocation);
	assert(active->allocation_count < ARRAY_SIZE(active->allocations));
	active->allocations[active->allocation_count++] = allocation;
	return allocation;
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

struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index)
{
	assert(type == IORESOURCE_MEM && index == 0);
	return pdev->resource;
}

unsigned long resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

void *devm_platform_ioremap_resource(struct platform_device *pdev,
				     unsigned int index)
{
	(void)pdev;
	assert(index == 0);
	return &active->source_register;
}

u32 readl(const void *address)
{
	assert(address == &active->source_register);
	return active->source_register;
}

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	assert(strcmp(name, "fplinux,ap-apb-syscon") == 0);
	return node->regmap;
}

int regmap_write(struct regmap *regmap, unsigned int offset, unsigned int value)
{
	assert(regmap == &active->ap_apb);
	regmap->writes++;
	regmap->offset = offset;
	regmap->written = value;
	if (regmap->error)
		return regmap->error;
	if (offset == 0x1000)
		regmap->value |= value;
	else {
		assert(offset == 0x2000);
		regmap->value &= ~value;
	}
	return 0;
}

int regmap_read(struct regmap *regmap, unsigned int offset, unsigned int *value)
{
	assert(regmap == &active->ap_apb && offset == 0);
	if (regmap->error)
		return regmap->error;
	*value = regmap->value;
	return 0;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	unsigned int index = active->registrations++;

	(void)dev;
	assert(index < 2);
	active->registered[index] = hw;
	active->init[index] = *hw->init;
	return 0;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data)
{
	struct of_phandle_args args = { .args_count = 1 };

	(void)dev;
	active->clocks = data;
	assert(active->clocks->num == 2);
	assert(get(&args, data) == active->registered[0]);
	args.args[0] = 1;
	assert(get(&args, data) == active->registered[1]);
	args.args[0] = 2;
	assert(PTR_ERR(get(&args, data)) == -EINVAL);
	return 0;
}

struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data)
{
	struct clk_hw_onecell_data *clocks = data;

	if (args->args_count != 1 || args->args[0] >= clocks->num)
		return ERR_PTR(-EINVAL);
	return clocks->hws[args->args[0]];
}

int main(void)
{
	struct fixture fixture = { .source_register = 0x80000000 };
	struct resource resource = { .start = 0x2150003c, .end = 0x2150003f };
	struct device_node node = { .regmap = &fixture.ap_apb };
	struct platform_device device = { .dev.of_node = &node,
					  .resource = &resource };
	const struct clk_ops *source_ops;
	const struct clk_ops *gate_ops;
	u32 unsupported[] = { 1, 2, 3, 0x100, 0x700, 0x701 };
	size_t index;

	active = &fixture;
	fixture.ap_apb.value = 0x80000100;
	assert(host_sc9820e_i2c2_clock_driver->probe(&device) == 0);
	assert(fixture.registrations == 2);
	source_ops = fixture.init[0].ops;
	gate_ops = fixture.init[1].ops;
	assert(source_ops->recalc_rate(fixture.registered[0], 26000000) ==
	       26000000);
	assert(!source_ops->enable && !source_ops->disable);
	assert(fixture.ap_apb.writes == 0);
	for (index = 0; index < ARRAY_SIZE(unsupported); index++) {
		fixture.source_register = unsupported[index];
		assert(source_ops->recalc_rate(fixture.registered[0],
					       26000000) == 0);
		assert(fixture.source_register == unsupported[index]);
	}
	fixture.source_register = 0;
	assert(source_ops->recalc_rate(fixture.registered[0], 0) == 0);
	assert(gate_ops->enable(fixture.registered[1]) == 0);
	assert(fixture.ap_apb.offset == 0x1000 &&
	       fixture.ap_apb.written == 0x400);
	assert(fixture.ap_apb.value == 0x80000500);
	assert(gate_ops->is_enabled(fixture.registered[1]) == 1);
	gate_ops->disable(fixture.registered[1]);
	assert(fixture.ap_apb.offset == 0x2000 &&
	       fixture.ap_apb.written == 0x400);
	assert(fixture.ap_apb.value == 0x80000100);
	assert(gate_ops->is_enabled(fixture.registered[1]) == 0);
	fixture.ap_apb.error = -EIO;
	assert(gate_ops->enable(fixture.registered[1]) == -EIO);
	assert(gate_ops->is_enabled(fixture.registered[1]) == -EIO);
	while (fixture.allocation_count)
		free(fixture.allocations[--fixture.allocation_count]);
	puts("PASS I2C2 inherited clock and gate host component");
	return 0;
}
