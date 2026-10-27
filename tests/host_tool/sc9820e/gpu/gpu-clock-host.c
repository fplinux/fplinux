// SPDX-License-Identifier: GPL-2.0-only
#include "compat/gpu-clock-host.h"
#include "sc27xx-regulator-core.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum fault {
	NO_FAULT,
	CORE_PD_READ,
	CORE_VSEL_READ,
	PMU_CONFIG_WRITE,
	GATE_SET,
	CLOCK_WRITE,
	QOS_WRITE,
	PMU_RESTORE,
};

struct regmap {
	u32 words[0x1000 / 4];
};

struct fixture {
	struct regmap pmic, pmu, aon;
	struct device parent;
	struct platform_device pdev;
	struct regulator supply;
	struct clk source;
	struct clk_hw source_hw;
	struct clk_hw *hw;
	struct clk_init_data init;
	u32 clock, qos, reset;
	u32 initial_pmu, initial_gate, initial_clock, initial_qos;
	u32 expected_core_uv;
	int property_error;
	unsigned int delays, control_reads, writes, gate_clears, errors;
	void *driver_allocation;
	enum fault fault;
};

static struct fixture *active;
extern struct platform_driver *test_driver_sc2721_regulator_driver;
extern struct platform_driver *test_driver_sc9820e_gpu_clock_driver;

static bool fail_once(enum fault fault)
{
	if (active->fault != fault)
		return false;
	active->fault = NO_FAULT;
	return true;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	assert(reg % 4 == 0 && reg < 0x1000);
	if (map == &active->pmic &&
	    ((reg == 0xc1c && fail_once(CORE_PD_READ)) ||
	     (reg == 0xc54 && fail_once(CORE_VSEL_READ))))
		return -EIO;
	*value = map->words[reg / 4];
	return 0;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	active->writes++;
	if (map == &active->aon) {
		assert(value == 0x08000000);
		if (reg == 0x1000) {
			map->words[0] |= value;
			/* An I/O error may be reported after a write took effect. */
			return fail_once(GATE_SET) ? -EIO : 0;
		}
		assert(reg == 0x2000);
		active->gate_clears++;
		map->words[0] &= ~value;
		return 0;
	}
	assert(map == &active->pmic && reg == 0xefc && value == 0x6e7f);
	map->words[reg / 4] = value;
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	assert(map == &active->pmu && reg == 0x20 && mask == 0x03ffffff);
	active->writes++;
	map->words[reg / 4] = (map->words[reg / 4] & ~mask) | (value & mask);
	if (value == 0x10101 && fail_once(PMU_CONFIG_WRITE))
		return -EIO;
	if (value == active->initial_pmu && fail_once(PMU_RESTORE))
		return -EIO;
	return 0;
}

const void *device_get_match_data(const struct device *dev)
{
	return dev->match_data;
}

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
	assert(!name);
	return dev->regmap;
}

int dev_err_probe(const struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

void dev_err(const struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
	active->errors++;
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	assert(dev == &active->pdev.dev && flags == 0);
	assert(!active->driver_allocation);
	active->driver_allocation = calloc(1, size);
	return active->driver_allocation;
}

struct clk *devm_clk_get(struct device *dev, const char *name)
{
	assert(dev == &active->pdev.dev && !strcmp(name, "source"));
	return &active->source;
}

struct regulator *devm_regulator_get(struct device *dev, const char *name)
{
	assert(dev == &active->pdev.dev && !strcmp(name, "core"));
	return &active->supply;
}

int device_property_read_u32(const struct device *dev, const char *name,
			     u32 *value)
{
	assert(dev == &active->pdev.dev);
	assert(!strcmp(name, "fplinux,expected-core-microvolt"));
	if (active->property_error)
		return active->property_error;
	*value = active->expected_core_uv;
	return 0;
}

struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name)
{
	(void)node;
	if (!strcmp(name, "fplinux,pmu-syscon"))
		return &active->pmu;
	assert(!strcmp(name, "fplinux,aon-syscon"));
	return &active->aon;
}

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	unsigned int index;

	assert(type == IORESOURCE_MEM);
	for (index = 0; index < 3; index++)
		if (!strcmp(pdev->resources[index].name, name))
			return &pdev->resources[index];
	return NULL;
}

unsigned long resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

void *devm_ioremap_resource(struct device *dev, struct resource *resource)
{
	assert(dev == &active->pdev.dev);
	if (!strcmp(resource->name, "clock"))
		return &active->clock;
	if (!strcmp(resource->name, "qos"))
		return &active->qos;
	assert(!strcmp(resource->name, "reset"));
	return &active->reset;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	assert(dev == &active->pdev.dev && !active->hw);
	assert(hw->init->num_parents == 1);
	assert(!strcmp(hw->init->parent_data[0].fw_name, "source"));
	assert(hw->init->flags == (CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE));
	active->init = *hw->init;
	active->hw = hw;
	hw->init = &active->init;
	/* CCF may query the initial rate during registration. */
	assert(hw->init->ops->recalc_rate(hw, active->source.rate) == 0);
	return 0;
}

struct clk_hw *of_clk_hw_simple_get(void *args, void *data)
{
	(void)args;
	return data;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clock_get get,
				void *data)
{
	assert(dev == &active->pdev.dev && get(NULL, data) == active->hw);
	return 0;
}

struct regulator_dev *
devm_regulator_register(struct device *dev, const struct regulator_desc *desc,
			const struct regulator_config *config)
{
	struct regulator_dev *rdev = calloc(1, sizeof(*rdev));

	assert(rdev && dev->num_resources < 32);
	rdev->desc = desc;
	rdev->config = *config;
	dev->resources[dev->num_resources++] = rdev;
	if (!strcmp(desc->name, "vddcore"))
		active->supply.rdev = rdev;
	return rdev;
}

struct regmap *rdev_get_regmap(struct regulator_dev *rdev)
{
	return rdev->config.regmap;
}

int regulator_is_enabled_regmap(struct regulator_dev *rdev)
{
	unsigned int value;
	int ret = regmap_read(rdev_get_regmap(rdev), rdev->desc->enable_reg,
			      &value);

	if (ret)
		return ret;
	return rdev->desc->enable_is_inverted ?
		       !(value & rdev->desc->enable_mask) :
		       !!(value & rdev->desc->enable_mask);
}

/* The unchanged SD operation table links these unused regulator helpers. */
int regulator_enable_regmap(struct regulator_dev *rdev)
{
	(void)rdev;
	abort();
}
int regulator_disable_regmap(struct regulator_dev *rdev)
{
	(void)rdev;
	abort();
}
int regulator_list_voltage_linear(struct regulator_dev *rdev,
				  unsigned int selector)
{
	(void)rdev;
	(void)selector;
	abort();
}
int regulator_map_voltage_linear(struct regulator_dev *rdev, int min_uV,
				 int max_uV)
{
	(void)rdev;
	(void)min_uV;
	(void)max_uV;
	abort();
}
int regulator_get_voltage_sel_regmap(struct regulator_dev *rdev)
{
	(void)rdev;
	abort();
}
int regulator_set_voltage_sel_regmap(struct regulator_dev *rdev,
				     unsigned int selector)
{
	(void)rdev;
	(void)selector;
	abort();
}

unsigned long clk_get_rate(struct clk *clock)
{
	return clock->rate;
}

int clk_enable(struct clk *clock)
{
	assert(clock->prepared);
	if (clock->error)
		return clock->error;
	clock->enables++;
	return 0;
}

void clk_disable(struct clk *clock)
{
	assert(clock->prepared && clock->enables);
	clock->enables--;
}

int regulator_get_voltage(struct regulator *regulator)
{
	return regulator->rdev->desc->ops->get_voltage(regulator->rdev);
}

static void assert_control_accessible(void)
{
	assert((active->pmu.words[0x20 / 4] & 0x03ffffff) == 0x10101);
	assert(active->aon.words[0] & 0x08000000);
	assert(active->source.enables && active->delays == 2);
}

u32 readl(const void *address)
{
	assert_control_accessible();
	active->control_reads++;
	if (address == &active->clock)
		return active->clock;
	if (address == &active->qos)
		return active->qos;
	assert(address == &active->reset);
	return active->reset;
}

void writel(u32 value, void *address)
{
	assert_control_accessible();
	active->writes++;
	if (address == &active->clock) {
		assert(!((value ^ active->clock) & ~7U));
		if (!fail_once(CLOCK_WRITE))
			active->clock = value;
		return;
	}
	assert(address == &active->qos);
	assert(!((value ^ active->qos) & ~0xff00U));
	if (!fail_once(QOS_WRITE))
		active->qos = value;
}

void udelay(unsigned int microseconds)
{
	assert(microseconds == 300 && active->delays < 2);
	active->delays++;
}

static void initialize(struct fixture *fixture, bool inherited_gate)
{
	*fixture = (struct fixture){
		.clock = 0xa4321002,
		.qos = 0xc321a5f0,
		.source = { .rate = 1535999999 },
		.expected_core_uv = 900000,
	};
	active = fixture;
	fixture->initial_pmu = fixture->pmu.words[0x20 / 4] = 0xc255aa66;
	fixture->initial_gate = fixture->aon.words[0] =
		inherited_gate ? 0x08400004 : 0x00400004;
	fixture->initial_clock = fixture->clock;
	fixture->initial_qos = fixture->qos;
	fixture->pmic.words[0xc54 / 4] = 0x5a0000a0;
	fixture->parent.regmap = &fixture->pmic;
	fixture->pdev.dev.parent = &fixture->parent;
	fixture->pdev.dev.match_data =
		test_driver_sc2721_regulator_driver->driver.of_match_table[0]
			.data;
	fixture->pdev.resources[0] = (struct resource){
		.start = 0x60100004,
		.end = 0x60100007,
		.name = "clock",
	};
	fixture->pdev.resources[1] = (struct resource){
		.start = 0x60100014,
		.end = 0x60100017,
		.name = "qos",
	};
	fixture->pdev.resources[2] = (struct resource){
		.start = 0x60100000,
		.end = 0x60100003,
		.name = "reset",
	};
}

static void release(struct fixture *fixture)
{
	unsigned int index;

	assert(!fixture->source.enables);
	for (index = 0; index < fixture->pdev.dev.num_resources; index++)
		free(fixture->pdev.dev.resources[index]);
	free(fixture->driver_allocation);
}

static void register_supply(struct fixture *fixture)
{
	assert(test_driver_sc2721_regulator_driver->probe(&fixture->pdev) == 0);
	assert(fixture->supply.rdev);
	assert(fixture->writes == 1);
	fixture->writes = 0;
}

static void register_clock(struct fixture *fixture)
{
	register_supply(fixture);
	assert(test_driver_sc9820e_gpu_clock_driver->probe(&fixture->pdev) ==
	       0);
	assert(fixture->hw && !fixture->control_reads && !fixture->writes);
}

static int prepare_clock(struct fixture *fixture)
{
	fixture->delays = 0;
	fixture->source.prepared = true;
	return fixture->init.ops->prepare(fixture->hw);
}

static void assert_restored(const struct fixture *fixture, bool unowned_changed)
{
	assert(fixture->pmu.words[0x20 / 4] ==
	       (fixture->initial_pmu ^ (unowned_changed ? 0x80000000 : 0)));
	assert(fixture->aon.words[0] ==
	       (fixture->initial_gate ^ (unowned_changed ? 4 : 0)));
	assert(fixture->clock ==
	       (fixture->initial_clock ^ (unowned_changed ? 0x80000000 : 0)));
	assert(fixture->qos ==
	       (fixture->initial_qos ^ (unowned_changed ? 1 : 0)));
	assert(!fixture->source.enables);
	assert(fixture->init.ops->recalc_rate(fixture->hw, 1535999999) == 0);
	assert(fixture->pmic.words[0xc54 / 4] == 0x5a0000a0);
	assert(!fixture->pmic.words[0xc1c / 4]);
}

static void lifetime_contract(void)
{
	unsigned int inherited_gate, changed;
	struct fixture fixture;

	for (inherited_gate = 0; inherited_gate < 2; inherited_gate++) {
		for (changed = 0; changed < 2; changed++) {
			initialize(&fixture, inherited_gate);
			register_clock(&fixture);
			assert(prepare_clock(&fixture) == 0);
			assert(fixture.init.ops->recalc_rate(
				       fixture.hw, 1535999999) == 255999999);
			assert(fixture.clock == 0xa4321000 &&
			       fixture.qos == 0xc32166f0);
			assert(fixture.source.enables == 1);
			if (changed) {
				fixture.clock ^= 0x80000000;
				fixture.qos ^= 1;
				fixture.pmu.words[0x20 / 4] ^= 0x80000000;
				fixture.aon.words[0] ^= 4;
			}
			fixture.init.ops->unprepare(fixture.hw);
			assert_restored(&fixture, changed);
			assert(fixture.gate_clears == (inherited_gate ? 0 : 1));
			assert(prepare_clock(&fixture) == 0);
			fixture.init.ops->unprepare(fixture.hw);
			assert_restored(&fixture, changed);
			assert(!fixture.errors);
			release(&fixture);
		}
	}
}

static void failure_contract(void)
{
	const enum fault faults[] = { CORE_PD_READ,	CORE_VSEL_READ,
				      PMU_CONFIG_WRITE, GATE_SET,
				      CLOCK_WRITE,	QOS_WRITE };
	struct fixture fixture;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(faults); index++) {
		initialize(&fixture, false);
		register_clock(&fixture);
		fixture.fault = faults[index];
		assert(prepare_clock(&fixture) == -EIO);
		assert(fixture.fault == NO_FAULT);
		assert_restored(&fixture, false);
		assert(prepare_clock(&fixture) == 0);
		fixture.init.ops->unprepare(fixture.hw);
		assert_restored(&fixture, false);
		release(&fixture);
	}
	initialize(&fixture, true);
	register_clock(&fixture);
	fixture.source.error = -EIO;
	assert(prepare_clock(&fixture) == -EIO);
	assert(!fixture.control_reads && !fixture.gate_clears);
	assert_restored(&fixture, false);
	release(&fixture);
	initialize(&fixture, false);
	register_clock(&fixture);
	fixture.reset = 1;
	assert(prepare_clock(&fixture) == -EBUSY);
	assert_restored(&fixture, false);
	release(&fixture);
	initialize(&fixture, false);
	fixture.initial_clock = fixture.clock |= 0x10;
	register_clock(&fixture);
	assert(prepare_clock(&fixture) == -EINVAL);
	assert_restored(&fixture, false);
	release(&fixture);
	initialize(&fixture, false);
	register_clock(&fixture);
	assert(prepare_clock(&fixture) == 0);
	fixture.fault = PMU_RESTORE;
	fixture.init.ops->unprepare(fixture.hw);
	assert(fixture.errors == 1 && fixture.fault == NO_FAULT);
	assert_restored(&fixture, false);
	release(&fixture);
}

static void registration_contract(void)
{
	struct fixture fixture;

	initialize(&fixture, false);
	register_clock(&fixture);
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) == 0);
	assert(!fixture.control_reads && !fixture.writes);
	fixture.source.rate = 0;
	assert(prepare_clock(&fixture) == -EINVAL);
	assert(!fixture.control_reads && !fixture.writes);
	fixture.source.rate = 1535999999;
	fixture.pmic.words[0xc54 / 4] = 0xa1;
	assert(prepare_clock(&fixture) == -EINVAL);
	assert(!fixture.control_reads && !fixture.writes);
	release(&fixture);
	initialize(&fixture, false);
	register_supply(&fixture);
	fixture.property_error = -EINVAL;
	assert(test_driver_sc9820e_gpu_clock_driver->probe(&fixture.pdev) ==
	       -EINVAL);
	assert(!fixture.hw && !fixture.control_reads && !fixture.writes);
	release(&fixture);
	initialize(&fixture, false);
	fixture.expected_core_uv = 0;
	register_supply(&fixture);
	assert(test_driver_sc9820e_gpu_clock_driver->probe(&fixture.pdev) ==
	       -EINVAL);
	assert(!fixture.hw && !fixture.control_reads && !fixture.writes);
	release(&fixture);
}

static void target_voltage_contract(void)
{
	struct fixture fixture;

	initialize(&fixture, false);
	fixture.expected_core_uv = 950000;
	register_clock(&fixture);
	assert(prepare_clock(&fixture) == -EINVAL);
	assert(!fixture.control_reads && !fixture.writes);
	assert_restored(&fixture, false);
	/* The same target expectation accepts a separately observed 950000 uV. */
	fixture.pmic.words[0xc54 / 4] = 0x5a0000b0;
	assert(prepare_clock(&fixture) == 0);
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) ==
	       255999999);
	fixture.init.ops->unprepare(fixture.hw);
	assert(fixture.pmu.words[0x20 / 4] == 0xc255aa66);
	assert(fixture.aon.words[0] == 0x00400004);
	assert(fixture.clock == 0xa4321002 && fixture.qos == 0xc321a5f0);
	assert(fixture.pmic.words[0xc54 / 4] == 0x5a0000b0);
	assert(!fixture.pmic.words[0xc1c / 4] && !fixture.source.enables);
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) == 0);
	assert(!fixture.errors);
	release(&fixture);
}

static void assigned_rate_contract(void)
{
	const struct {
		unsigned long requested_hz, actual_hz;
		u32 clock;
	} cases[] = {
		{ 256000000, 255999999, 0xa4321000 },
		{ 384000000, 383999999, 0xa4321002 },
		{ 512000000, 511999999, 0xa4321003 },
	};
	struct fixture fixture;
	struct clk_rate_request request;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		initialize(&fixture, false);
		register_clock(&fixture);
		request = (struct clk_rate_request){
			.rate = cases[index].requested_hz,
			.max_rate = ULONG_MAX,
			.best_parent_rate = fixture.source.rate,
			.best_parent_hw = &fixture.source_hw,
		};
		assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
		       0);
		assert(request.rate == cases[index].actual_hz);
		assert(request.best_parent_rate == 1535999999);
		assert(request.best_parent_hw == &fixture.source_hw);
		assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
		       0);
		assert(request.rate == cases[index].actual_hz);
		assert(fixture.init.ops->set_rate(fixture.hw, request.rate,
						  request.best_parent_rate) ==
		       0);
		assert(fixture.init.ops->recalc_rate(
			       fixture.hw, request.best_parent_rate) == 0);
		assert(!fixture.control_reads && !fixture.writes);
		assert(prepare_clock(&fixture) == 0);
		assert(fixture.clock == cases[index].clock);
		assert(fixture.init.ops->recalc_rate(
			       fixture.hw, request.best_parent_rate) ==
		       cases[index].actual_hz);
		assert(fixture.source.rate == 1535999999);
		fixture.init.ops->unprepare(fixture.hw);
		assert_restored(&fixture, false);
		assert(prepare_clock(&fixture) == 0);
		assert(fixture.clock == cases[index].clock);
		fixture.init.ops->unprepare(fixture.hw);
		assert_restored(&fixture, false);
		release(&fixture);
	}
	initialize(&fixture, false);
	fixture.source.rate = 1536000000;
	register_clock(&fixture);
	request = (struct clk_rate_request){
		.rate = 384000000,
		.max_rate = ULONG_MAX,
		.best_parent_rate = fixture.source.rate,
		.best_parent_hw = &fixture.source_hw,
	};
	assert(fixture.init.ops->determine_rate(fixture.hw, &request) == 0);
	assert(request.rate == 384000000);
	assert(request.best_parent_rate == 1536000000);
	assert(request.best_parent_hw == &fixture.source_hw);
	assert(fixture.init.ops->set_rate(fixture.hw, request.rate,
					  request.best_parent_rate) == 0);
	assert(prepare_clock(&fixture) == 0);
	assert(fixture.clock == 0xa4321002);
	assert(fixture.init.ops->recalc_rate(
		       fixture.hw, request.best_parent_rate) == 384000000);
	fixture.init.ops->unprepare(fixture.hw);
	assert_restored(&fixture, false);
	release(&fixture);
}

static void rate_admission_contract(void)
{
	const unsigned long unsupported[] = { 0, 307200000, 600000000,
					      680000000 };
	struct fixture fixture;
	struct clk_rate_request request;
	unsigned int index, reads, writes;

	initialize(&fixture, false);
	register_clock(&fixture);
	assert(fixture.init.ops->set_rate(fixture.hw, 511999999, 1535999999) ==
	       0);
	request = (struct clk_rate_request){
		.max_rate = ULONG_MAX,
		.best_parent_rate = fixture.source.rate,
		.best_parent_hw = &fixture.source_hw,
	};
	/* These are provider callbacks, not a complete CCF rate transaction. */
	for (index = 0; index < ARRAY_SIZE(unsupported); index++) {
		request.rate = unsupported[index];
		assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
		       -EINVAL);
		assert(request.rate == unsupported[index]);
		assert(request.best_parent_rate == 1535999999);
		assert(request.best_parent_hw == &fixture.source_hw);
		assert(fixture.init.ops->set_rate(fixture.hw,
						  unsupported[index],
						  1535999999) == -EINVAL);
	}
	request.rate = 384000000;
	request.best_parent_rate = 0;
	assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
	       -EINVAL);
	assert(fixture.init.ops->set_rate(fixture.hw, 384000000, 0) == -EINVAL);
	request.best_parent_rate = 1535999999;
	request.min_rate = 384000000;
	assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
	       -EINVAL);
	assert(request.rate == 384000000);
	request.min_rate = 0;
	request.max_rate = 383999998;
	assert(fixture.init.ops->determine_rate(fixture.hw, &request) ==
	       -EINVAL);
	assert(request.rate == 384000000);
	request.min_rate = request.max_rate = 383999999;
	assert(fixture.init.ops->determine_rate(fixture.hw, &request) == 0);
	assert(request.rate == 383999999);
	assert(request.best_parent_rate == 1535999999);
	assert(request.best_parent_hw == &fixture.source_hw);
	assert(!fixture.control_reads && !fixture.writes);
	assert(prepare_clock(&fixture) == 0);
	assert(fixture.clock == 0xa4321003);
	reads = fixture.control_reads;
	writes = fixture.writes;
	assert(fixture.init.ops->set_rate(fixture.hw, 383999999, 1535999999) ==
	       -EBUSY);
	assert(fixture.control_reads == reads && fixture.writes == writes);
	assert(fixture.clock == 0xa4321003);
	fixture.init.ops->unprepare(fixture.hw);
	assert_restored(&fixture, false);
	release(&fixture);
}

static void rate_observation_contract(void)
{
	struct fixture fixture;

	initialize(&fixture, false);
	register_clock(&fixture);
	assert(prepare_clock(&fixture) == 0);
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) ==
	       255999999);
	/* The fake control bank can expose a different physical selection. */
	fixture.clock = 0xa4321002;
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) ==
	       383999999);
	fixture.clock = 0xa4321003;
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) ==
	       511999999);
	fixture.clock = 0xa4321013;
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) ==
	       255999999);
	fixture.clock = 0xa4321004;
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1535999999) == 0);
	fixture.clock = 0xa4321000;
	assert(fixture.init.ops->recalc_rate(fixture.hw, 1536000000) ==
	       256000000);
	fixture.init.ops->unprepare(fixture.hw);
	assert_restored(&fixture, false);
	release(&fixture);
}

static void selected_rate_failure_contract(void)
{
	const unsigned long rates[] = { 383999999, 511999999 };
	const enum fault faults[] = { CLOCK_WRITE, QOS_WRITE };
	struct fixture fixture;
	unsigned int rate_index, fault_index;

	for (rate_index = 0; rate_index < ARRAY_SIZE(rates); rate_index++) {
		for (fault_index = 0; fault_index < ARRAY_SIZE(faults);
		     fault_index++) {
			initialize(&fixture, false);
			fixture.initial_clock = fixture.clock = 0xa4321000;
			register_clock(&fixture);
			assert(fixture.init.ops->set_rate(fixture.hw,
							  rates[rate_index],
							  1535999999) == 0);
			fixture.fault = faults[fault_index];
			assert(prepare_clock(&fixture) == -EIO);
			assert(fixture.fault == NO_FAULT);
			assert_restored(&fixture, false);
			assert(prepare_clock(&fixture) == 0);
			assert(fixture.init.ops->recalc_rate(fixture.hw,
							     1535999999) ==
			       rates[rate_index]);
			fixture.init.ops->unprepare(fixture.hw);
			assert_restored(&fixture, false);
			release(&fixture);
		}
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "lifetime"))
		lifetime_contract();
	else if (!strcmp(argv[1], "failure"))
		failure_contract();
	else if (!strcmp(argv[1], "registration"))
		registration_contract();
	else if (!strcmp(argv[1], "target-voltage"))
		target_voltage_contract();
	else if (!strcmp(argv[1], "assigned-rate"))
		assigned_rate_contract();
	else if (!strcmp(argv[1], "rate-admission"))
		rate_admission_contract();
	else if (!strcmp(argv[1], "rate-observation"))
		rate_observation_contract();
	else if (!strcmp(argv[1], "selected-rate-failure"))
		selected_rate_failure_contract();
	else
		return 2;
	puts("GPU clock ownership and read-only core supply contracts passed");
	return 0;
}
