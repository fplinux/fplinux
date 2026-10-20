// SPDX-License-Identifier: GPL-2.0-only
#include "cpu-clock-host.h"
#include "spi-sprd-r4p0-clock.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
	u32 control;
	void *allocation;
	struct clk_hw *clock;
	struct clk_init_data init;
	struct clk_hw parents[4];
	unsigned long parent_rates[4];
	unsigned int selected_parent;
	unsigned int writes;
	bool fail_allocation;
	bool suppress_write;
	bool missing_parent;
	int map_error;
	int registration_error;
	int provider_error;
};

static struct fixture *active;

u32 readl(const void *address)
{
	assert(address == &active->control);
	return active->control;
}

void writel(u32 value, void *address)
{
	assert(address == &active->control);
	active->writes++;
	if (!active->suppress_write)
		active->control = value;
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	(void)dev;
	(void)flags;
	assert(!active->allocation);
	if (active->fail_allocation)
		return NULL;
	active->allocation = calloc(1, size);
	assert(active->allocation);
	return active->allocation;
}

struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index)
{
	assert(type == IORESOURCE_MEM && index == 0);
	return &pdev->resources[0];
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
	return active->map_error ? ERR_PTR(active->map_error) :
				   &active->control;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	static const char *const names[] = { "ext26m", "twpll128m",
					     "twpll153m6", "twpll192m" };
	unsigned int index;

	(void)dev;
	if (active->registration_error)
		return active->registration_error;
	assert(!active->clock);
	assert(hw->init->num_parents == 4);
	for (index = 0; index < 4; index++)
		assert(!strcmp(hw->init->parent_data[index].fw_name,
			       names[index]));
	active->clock = hw;
	active->init = *hw->init;
	return 0;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data)
{
	struct of_phandle_args args = {};

	(void)dev;
	if (active->provider_error)
		return active->provider_error;
	assert(get(&args, data) == active->clock);
	return 0;
}

struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *args, void *data)
{
	(void)args;
	return data;
}

struct clk_hw *clk_hw_get_parent(const struct clk_hw *hw)
{
	assert(hw == active->clock);
	return active->missing_parent ?
		       NULL :
		       &active->parents[active->selected_parent];
}

unsigned long clk_hw_get_rate(const struct clk_hw *hw)
{
	unsigned int index;

	for (index = 0; index < 4; index++)
		if (hw == &active->parents[index])
			return active->parent_rates[index];
	abort();
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

static struct platform_device device(void)
{
	return (struct platform_device){
		.resources = { { .start = 0x21500050, .end = 0x21500053 } },
	};
}

static void initialize(struct fixture *fixture)
{
	*fixture = (struct fixture){
		.control = 0xa5000703,
		.parent_rates = { 26000000, 127999999, 153599999, 191999999 },
	};
	active = fixture;
}

static const struct clk_ops *probe(struct fixture *fixture)
{
	struct platform_device pdev = device();

	assert(host_sc9820e_hs_spi_clock_driver->probe(&pdev) == 0);
	assert(fixture->writes == 0);
	fixture->selected_parent =
		fixture->init.ops->get_parent(fixture->clock);
	return fixture->init.ops;
}

static void release(struct fixture *fixture)
{
	free(fixture->allocation);
	fixture->allocation = NULL;
	fixture->clock = NULL;
}

static void check_mux_and_divider(void)
{
	static const unsigned long divided_rates[] = { 3250000, 15999999,
						       19199999, 23999999 };
	struct fixture fixture;
	const struct clk_ops *ops;
	unsigned int index;

	initialize(&fixture);
	ops = probe(&fixture);
	for (index = 0; index < 4; index++) {
		fixture.control = 0xa5000700 | index;
		assert(ops->get_parent(fixture.clock) == index);
		assert(ops->recalc_rate(fixture.clock,
					fixture.parent_rates[index]) ==
		       divided_rates[index]);
		assert(ops->set_parent(fixture.clock, 3 - index) == 0);
		assert(fixture.control == (0xa5000700 | (3 - index)));
		fixture.selected_parent = 3 - index;
		assert(ops->set_rate(fixture.clock,
				     fixture.parent_rates[3 - index],
				     fixture.parent_rates[3 - index]) == 0);
		assert(fixture.control == (0xa5000000 | (3 - index)));
		assert(ops->recalc_rate(fixture.clock,
					fixture.parent_rates[3 - index]) ==
		       fixture.parent_rates[3 - index]);
	}
	assert(fixture.writes == 8);
	release(&fixture);
}

static void check_parent_rate_request(void)
{
	struct fixture fixture;
	const struct clk_ops *ops;
	struct clk_rate_request req;
	u32 divider = 99, effective_hz = 99;

	initialize(&fixture);
	ops = probe(&fixture);
	assert(ops->set_parent(fixture.clock, 3) == 0);
	fixture.selected_parent = 3;
	req = (struct clk_rate_request){ .rate = 191999999,
					 .min_rate = 191999999,
					 .max_rate = 191999999 };
	assert(ops->determine_rate(fixture.clock, &req) == 0);
	assert(req.best_parent_hw == &fixture.parents[3]);
	assert(req.best_parent_rate == 191999999 && req.rate == 191999999);
	assert(ops->set_rate(fixture.clock, req.rate, req.best_parent_rate) ==
	       0);
	assert(fixture.control == 0xa5000003);
	assert(sprd_spi_r4p0_clock_params(req.rate, 95999999, &divider,
					  &effective_hz) == 0);
	assert(divider == 0 && effective_hz == 95999999);
	assert(fixture.writes == 2);

	req.min_rate = 192000000;
	req.max_rate = UINT32_MAX;
	assert(ops->determine_rate(fixture.clock, &req) == -EINVAL);
	req.min_rate = 0;
	req.max_rate = 191999998;
	req.rate = 191999998;
	assert(ops->determine_rate(fixture.clock, &req) == -EINVAL);
	req.max_rate = UINT32_MAX;
	fixture.parent_rates[3] = 0;
	assert(ops->determine_rate(fixture.clock, &req) == -EINVAL);
	fixture.parent_rates[3] = 191999999;
	fixture.missing_parent = true;
	assert(ops->determine_rate(fixture.clock, &req) == -EINVAL);
	assert(fixture.writes == 2);
	release(&fixture);
}

static void check_write_errors(void)
{
	struct fixture fixture;
	const struct clk_ops *ops;

	initialize(&fixture);
	ops = probe(&fixture);
	assert(ops->set_parent(fixture.clock, 4) == -EINVAL);
	assert(ops->set_rate(fixture.clock, 0, 0) == -EINVAL);
	assert(ops->set_rate(fixture.clock, 13000000, 26000000) == -EINVAL);
	assert(fixture.writes == 0 && fixture.control == 0xa5000703);
	fixture.suppress_write = true;
	assert(ops->set_parent(fixture.clock, 0) == -EIO);
	assert(ops->set_rate(fixture.clock, 191999999, 191999999) == -EIO);
	assert(fixture.writes == 2 && fixture.control == 0xa5000703);
	release(&fixture);
}

static void check_probe_errors(void)
{
	unsigned int scenario;

	for (scenario = 0; scenario < 5; scenario++) {
		struct fixture fixture;
		struct platform_device pdev = device();
		int expected = -EIO;

		initialize(&fixture);
		switch (scenario) {
		case 0:
			pdev.resources[0].end--;
			expected = -EINVAL;
			break;
		case 1:
			fixture.fail_allocation = true;
			expected = -ENOMEM;
			break;
		case 2:
			fixture.map_error = -EIO;
			break;
		case 3:
			fixture.registration_error = -EIO;
			break;
		case 4:
			fixture.provider_error = -EIO;
			break;
		}
		assert(host_sc9820e_hs_spi_clock_driver->probe(&pdev) ==
		       expected);
		assert(fixture.writes == 0 && fixture.control == 0xa5000703);
		release(&fixture);
	}
}

static void check_spi_rates(void)
{
	static const struct {
		u32 source_hz, requested_hz, divider, effective_hz;
	} cases[] = {
		{ 26000000, 13000000, 0, 13000000 },
		{ 26000000, 10000000, 1, 6500000 },
		{ 191999999, 95999999, 0, 95999999 },
		{ 191999999, 48000000, 1, 47999999 },
		{ 191999999, 13000000, 7, 11999999 },
		{ 3, 1, 0, 1 },
		{ 2, 1, 0, 1 },
		{ 4294967295U, 2147483647, 0, 2147483647 },
		{ 4294967295U, 1, 2147483646, 1 },
	};
	static const u32 invalid[][2] = {
		{ 0, 1 },
		{ 1, 1 },
		{ 26000000, 0 },
		{ 26000000, 13000001 },
		{ 191999999, 96000000 },
	};
	u32 divider, effective_hz;
	size_t index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		divider = effective_hz = 99;
		assert(sprd_spi_r4p0_clock_params(cases[index].source_hz,
						  cases[index].requested_hz,
						  &divider,
						  &effective_hz) == 0);
		assert(divider == cases[index].divider);
		assert(effective_hz == cases[index].effective_hz);
	}
	for (index = 0; index < ARRAY_SIZE(invalid); index++) {
		divider = effective_hz = 99;
		assert(sprd_spi_r4p0_clock_params(invalid[index][0],
						  invalid[index][1], &divider,
						  &effective_hz) == -EINVAL);
		assert(divider == 99 && effective_hz == 99);
	}
}

static void check_probe_ownership(void)
{
	struct fixture fixture;
	struct platform_device pdev = device();
	const struct clk_ops *ops;

	initialize(&fixture);
	fixture.provider_error = -EIO;
	assert(host_sc9820e_hs_spi_clock_driver->probe(&pdev) == -EIO);
	assert(fixture.allocation && fixture.clock && fixture.writes == 0);
	/* The peer releases the resources owned by devm after a failed probe. */
	release(&fixture);
	initialize(&fixture);
	ops = probe(&fixture);
	assert(ops->get_parent(fixture.clock) == 3);
	assert(ops->set_parent(fixture.clock, 1) == 0);
	assert(ops->set_rate(fixture.clock, 127999999, 127999999) == 0);
	assert(fixture.control == 0xa5000001 && fixture.writes == 2);
	release(&fixture);
	initialize(&fixture);
	ops = probe(&fixture);
	assert(ops->get_parent(fixture.clock) == 3 && fixture.writes == 0);
	release(&fixture);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "mux-divider"))
		check_mux_and_divider();
	else if (!strcmp(argv[1], "parent-rate-request"))
		check_parent_rate_request();
	else if (!strcmp(argv[1], "write-errors"))
		check_write_errors();
	else if (!strcmp(argv[1], "probe-errors"))
		check_probe_errors();
	else if (!strcmp(argv[1], "probe-ownership"))
		check_probe_ownership();
	else {
		assert(!strcmp(argv[1], "spi-rates"));
		check_spi_rates();
	}
	return 0;
}
