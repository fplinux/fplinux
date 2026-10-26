// SPDX-License-Identifier: GPL-2.0-only
#include "gate-compat/gate-kernel.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The doubles replace kernel registration and regmap, not gate callbacks. */
struct regmap {
	unsigned int status;
	unsigned int read_offset;
	unsigned int write_offset;
	unsigned int write_value;
	unsigned int reads;
	unsigned int writes;
	int read_error;
	int write_error;
};

struct gate_fixture {
	struct regmap regmap;
	struct regmap other_regmap;
	void *allocations[4];
	size_t allocation_count;
	struct clk_hw *gate;
	struct clk_init_data gate_init;
	struct clk_hw mux;
	host_clk_get provider_get;
	void *provider_data;
	const char *syscon_name;
	unsigned int mux_register;
	unsigned int diagnostics;
	char diagnostic[160];
	const char *mux_name;
	int lookup_error;
	int registration_error;
	int provider_error;
};

struct gate_case {
	const char *argument;
	struct platform_driver *driver;
	const char *compatible;
	const char *clock_name;
	unsigned int offset;
	unsigned int mask;
	const char *syscon_property;
	bool sdio;
};

static struct gate_fixture *active;

const void *device_get_match_data(const struct device *dev)
{
	return dev->match_data;
}

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

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	active->syscon_name = name;
	if (active->lookup_error)
		return ERR_PTR(active->lookup_error);
	if (strcmp(name, "fplinux,aon-syscon") == 0)
		return node->regmap;
	if (strcmp(name, "fplinux,ap-ahb-syscon") == 0)
		return node->ap_ahb_regmap;
	return ERR_PTR(-ENOENT);
}

int regmap_write(struct regmap *regmap, unsigned int offset, unsigned int value)
{
	regmap->writes++;
	regmap->write_offset = offset;
	regmap->write_value = value;
	if (regmap->write_error)
		return regmap->write_error;
	/* The fake models the hardware's write-one set and clear aliases. */
	if (offset >= 0x1000 && offset < 0x2000)
		regmap->status |= value;
	else if (offset >= 0x2000 && offset < 0x3000)
		regmap->status &= ~value;
	else
		return -EINVAL;
	return 0;
}

int regmap_read(struct regmap *regmap, unsigned int offset, unsigned int *value)
{
	regmap->reads++;
	regmap->read_offset = offset;
	if (regmap->read_error)
		return regmap->read_error;
	*value = regmap->status;
	return 0;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	(void)dev;
	if (active->registration_error)
		return active->registration_error;
	assert(!active->gate);
	active->gate = hw;
	/* The kernel consumes init during registration; the probe owns its storage. */
	active->gate_init = *hw->init;
	return 0;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data)
{
	(void)dev;
	if (active->provider_error)
		return active->provider_error;
	active->provider_get = get;
	active->provider_data = data;
	return 0;
}

struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *args, void *data)
{
	(void)args;
	return data;
}

struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data)
{
	struct clk_hw_onecell_data *clocks = data;

	if (args->args_count != 1 || args->args[0] >= clocks->num)
		return ERR_PTR(-EINVAL);
	return clocks->hws[args->args[0]];
}

struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index)
{
	assert(type == IORESOURCE_MEM);
	assert(index == 0);
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
	return &active->mux_register;
}

struct clk_hw *devm_clk_hw_register_mux_parent_data_table(
	struct device *dev, const char *name,
	const struct clk_parent_data *parents, u8 num_parents,
	unsigned long flags, void *reg, u8 shift, u8 width, u8 mux_flags,
	const u32 *table, void *lock)
{
	(void)dev;
	assert(strcmp(name, active->mux_name) == 0);
	assert(num_parents == 2);
	assert(strcmp(parents[0].fw_name, "ext1m") == 0);
	assert(strcmp(parents[1].fw_name, "ext26m") == 0);
	assert(flags == (CLK_SET_RATE_NO_REPARENT | CLK_GET_RATE_NOCACHE));
	assert(reg == &active->mux_register);
	assert(shift == 0);
	assert(width == 3);
	assert(mux_flags == 0);
	assert(table == NULL);
	assert(lock == NULL);
	return &active->mux;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

void dev_err(struct device *dev, const char *format, ...)
{
	va_list arguments;

	(void)dev;
	active->diagnostics++;
	va_start(arguments, format);
	vsnprintf(active->diagnostic, sizeof(active->diagnostic), format,
		  arguments);
	va_end(arguments);
}

static void release_fixture(struct gate_fixture *fixture)
{
	while (fixture->allocation_count)
		free(fixture->allocations[--fixture->allocation_count]);
}

static void prepare_device(const struct gate_case *test,
			   struct platform_device *device)
{
	const struct of_device_id *match = test->driver->driver.of_match_table;
	struct device_node *node = device->dev.of_node;

	active->other_regmap.status = 0xdeadbeef;
	active->mux_name = strcmp(test->argument, "emmc") == 0 ?
				   "sc9820e-emmc-mux" :
				   "sc9820e-sdio0-mux";
	if (strcmp(test->syscon_property, "fplinux,aon-syscon") == 0) {
		node->regmap = &active->regmap;
		node->ap_ahb_regmap = &active->other_regmap;
	} else {
		node->regmap = &active->other_regmap;
		node->ap_ahb_regmap = &active->regmap;
	}

	for (; match->compatible; match++) {
		if (strcmp(match->compatible, test->compatible) == 0) {
			device->dev.match_data = match->data;
			return;
		}
	}
	assert(!"clock provider does not match its consumer compatible");
}

static void check_provider(const struct gate_case *test)
{
	struct of_phandle_args args = {};
	struct clk_hw *gate;

	assert(active->gate);
	assert(strcmp(active->gate_init.name, test->clock_name) == 0);
	assert(strcmp(active->syscon_name, test->syscon_property) == 0);
	if (test->sdio) {
		assert(active->provider_get == of_clk_hw_onecell_get);
		args.args_count = 1;
		args.args[0] = 0;
		assert(active->provider_get(&args, active->provider_data) ==
		       &active->mux);
		args.args[0] = 1;
		gate = active->provider_get(&args, active->provider_data);
		args.args[0] = 2;
		assert(PTR_ERR(active->provider_get(
			       &args, active->provider_data)) == -EINVAL);
	} else {
		assert(active->provider_get == of_clk_hw_simple_get);
		gate = active->provider_get(&args, active->provider_data);
	}
	assert(gate == active->gate);
}

static void check_gate_callbacks(const struct gate_case *test)
{
	const struct clk_ops *ops = active->gate_init.ops;
	struct regmap *regmap = &active->regmap;
	const char *diagnostic_name = test->sdio ? "SDIO0 AHB" :
						   test->clock_name;
	unsigned int unrelated = 0xa5a55a5a & ~test->mask;
	unsigned int reads;

	if (strcmp(test->argument, "emmc") == 0)
		diagnostic_name = "eMMC AHB";

	assert(ops->enable && ops->disable && ops->is_enabled);
	assert(regmap->writes == 0 && regmap->reads == 0);
	regmap->status = unrelated;

	assert(ops->enable(active->gate) == 0);
	assert(regmap->writes == 1);
	assert(regmap->write_offset == test->offset + 0x1000);
	assert(regmap->write_value == test->mask);
	assert(regmap->status == (unrelated | test->mask));
	regmap->write_error = -ETIMEDOUT;
	assert(ops->enable(active->gate) == -ETIMEDOUT);
	assert(regmap->writes == 2);
	assert(regmap->write_offset == test->offset + 0x1000);
	assert(regmap->write_value == test->mask);
	assert(regmap->status == (unrelated | test->mask));
	assert(regmap->reads == 0);

	regmap->status = 0xffffffff;
	assert(ops->is_enabled(active->gate) == 1);
	regmap->status = 0xffffffff ^ test->mask;
	assert(ops->is_enabled(active->gate) == 0);
	regmap->status = test->mask;
	assert(ops->is_enabled(active->gate) == 1);
	assert(regmap->read_offset == test->offset);
	regmap->read_error = -EIO;
	assert(ops->is_enabled(active->gate) == -EIO);
	assert(regmap->read_offset == test->offset);
	assert(regmap->writes == 2);
	reads = regmap->reads;

	regmap->write_error = 0;
	regmap->status = unrelated | test->mask;
	ops->disable(active->gate);
	assert(regmap->writes == 3);
	assert(regmap->write_offset == test->offset + 0x2000);
	assert(regmap->write_value == test->mask);
	assert(regmap->status == unrelated);
	assert(active->diagnostics == 0);
	regmap->write_error = -EIO;
	regmap->status = unrelated | test->mask;
	ops->disable(active->gate);
	assert(regmap->writes == 4);
	assert(regmap->write_offset == test->offset + 0x2000);
	assert(regmap->write_value == test->mask);
	assert(regmap->status == (unrelated | test->mask));
	assert(regmap->reads == reads);
	assert(active->diagnostics == 1);
	assert(strstr(active->diagnostic, "disable"));
	assert(strstr(active->diagnostic, diagnostic_name));
	assert(strstr(active->diagnostic, "-5"));
}

static void check_probe(const struct gate_case *test)
{
	struct gate_fixture fixture = {};
	struct device_node node = { .regmap = &fixture.regmap };
	struct resource resource = { .start = 0, .end = 3 };
	struct platform_device device = {
		.dev = { .of_node = &node },
		.resource = &resource,
	};

	active = &fixture;
	prepare_device(test, &device);
	assert(test->driver->probe(&device) == 0);
	check_provider(test);
	check_gate_callbacks(test);
	assert(fixture.other_regmap.reads == 0);
	assert(fixture.other_regmap.writes == 0);
	assert(fixture.other_regmap.status == 0xdeadbeef);
	release_fixture(&fixture);
}

static void check_probe_errors(const struct gate_case *test)
{
	struct gate_fixture fixture = {};
	struct device_node node = { .regmap = &fixture.regmap };
	struct resource resource = { .start = 0, .end = 3 };
	struct platform_device device = {
		.dev = { .of_node = &node },
		.resource = &resource,
	};

	active = &fixture;
	prepare_device(test, &device);
	fixture.lookup_error = -EAGAIN;
	assert(test->driver->probe(&device) == -EAGAIN);
	assert(!fixture.gate && !fixture.provider_get);
	release_fixture(&fixture);
	fixture.lookup_error = 0;
	fixture.registration_error = -ENOSPC;
	assert(test->driver->probe(&device) == -ENOSPC);
	assert(!fixture.gate && !fixture.provider_get);
	release_fixture(&fixture);
	fixture.registration_error = 0;
	fixture.provider_error = -EBUSY;
	assert(test->driver->probe(&device) == -EBUSY);
	assert(!fixture.provider_get);
	assert(fixture.regmap.writes == 0);
	release_fixture(&fixture);
}

int main(int argc, char **argv)
{
	const struct gate_case cases[] = {
		{ "hwspinlock", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-aon-gate", "sc9820e-hwspinlock", 0x0,
		  0x00400000, "fplinux,aon-syscon", false },
		{ "hs-spi", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-hs-spi-gate", "sc9820e-hs-spi-enable", 0x134,
		  0x00000200, "fplinux,aon-syscon", false },
		{ "gpio", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-gpio-gate", "sc9820e-gpio", 0x0, 0x00000008,
		  "fplinux,aon-syscon", false },
		{ "ap-dma", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-ap-dma-gate", "sc9820e-ap-dma-enable", 0x0,
		  0x00000020, "fplinux,ap-ahb-syscon", false },
		{ "efuse", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-efuse-gate", "sc9820e-efuse", 0x0,
		  0x00002000, "fplinux,aon-syscon", false },
		{ "mailbox", host_sc9820e_peripheral_gates_driver,
		  "fplinux,sc9820e-mailbox-gate", "sc9820e-mailbox", 0x4,
		  0x00200000, "fplinux,aon-syscon", false },
		{ "sdio", host_sc9820e_sdio_clock_driver,
		  "fplinux,sc9820e-sdio-clock", "sc9820e-sdio0-ahb", 0x0,
		  0x00000100, "fplinux,ap-ahb-syscon", true },
		{ "emmc", host_sc9820e_sdio_clock_driver,
		  "fplinux,sc9820e-emmc-clock", "sc9820e-emmc-ahb", 0x0,
		  0x00000800, "fplinux,ap-ahb-syscon", true },
	};
	size_t index;

	assert(argc == 2);
	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		if (strcmp(argv[1], cases[index].argument) != 0)
			continue;
		check_probe(&cases[index]);
		check_probe_errors(&cases[index]);
		puts("SC9820E clock provider host callback cases passed");
		return 0;
	}
	return 2;
}
