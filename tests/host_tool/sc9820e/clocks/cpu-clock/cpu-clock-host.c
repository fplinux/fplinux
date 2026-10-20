// SPDX-License-Identifier: GPL-2.0-only
#include "cpu-clock-host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Doubles replace registration, resource maps, MMIO and cpufreq interfaces. */
struct regmap {
	u32 selection;
	u32 divider;
	u32 fail_offset;
	int read_error;
};

struct fixture {
	struct regmap ap_ahb;
	u32 mpll[2];
	u32 twpll[2];
	void *allocations[2];
	unsigned int allocation_count;
	unsigned int allocation_calls;
	unsigned int fail_allocation_call;
	struct clk_hw *clock;
	struct clk_init_data init;
	struct clk_hw *twpll_clock;
	struct clk_init_data twpll_init;
	int lookup_error;
	int map_error;
	int registration_error;
	unsigned int registration_calls;
	unsigned int fail_registration_call;
	int provider_error;
	unsigned int writes;
	struct cpufreq_driver *cpufreq;
	struct device_node *provider_node;
	struct device_node cpu_nodes[2];
	struct device cpu_devices[2];
	struct clk_hw reference;
	unsigned long reference_hz;
	unsigned int domain_members;
	unsigned int cpu_clock_outputs[2];
	int cpu_clock_args_count[2];
	int cpu_clock_errors[2];
	int cpufreq_error;
	int action_error;
	void (*cleanup)(void *);
	void *cleanup_data;
	unsigned int unregisters;
	int write_error;
	bool apply_failed_write;
	bool suppress_write;
	bool corrupt_divider_after_write;
	int read_error_after_write;
	unsigned int selection_reads;
	unsigned int mutate_selection_read;
};

static struct fixture *active;

u32 readl(const void *address)
{
	assert(address == &active->mpll[0] || address == &active->mpll[1] ||
	       address == &active->twpll[0] || address == &active->twpll[1]);
	return *(const u32 *)address;
}

void writel(u32 value, void *address)
{
	active->writes++;
	*(u32 *)address = value;
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	(void)dev;
	(void)flags;
	if (++active->allocation_calls == active->fail_allocation_call)
		return NULL;
	assert(active->allocation_count < ARRAY_SIZE(active->allocations));
	active->allocations[active->allocation_count] = calloc(1, size);
	assert(active->allocations[active->allocation_count]);
	return active->allocations[active->allocation_count++];
}

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	assert(strcmp(name, "fplinux,ap-ahb-syscon") == 0);
	if (active->lookup_error)
		return ERR_PTR(active->lookup_error);
	return node->regmap;
}

int regmap_read(struct regmap *map, unsigned int offset, unsigned int *value)
{
	assert(offset == 0x54 || offset == 0x38);
	if (offset == 0x54 &&
	    ++active->selection_reads == active->mutate_selection_read)
		map->selection ^= 0x80;
	*value = offset == 0x54 ? map->selection : map->divider;
	/* An ignored error must not pass by leaving an invalid zero-rate input. */
	if (map->read_error && map->fail_offset == offset)
		return map->read_error;
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int offset,
		       unsigned int mask, unsigned int value)
{
	assert(offset == 0x54 && mask == 3 && (value == 2 || value == 3));
	active->writes++;
	if ((!active->write_error || active->apply_failed_write) &&
	    !active->suppress_write)
		map->selection = (map->selection & ~mask) | value;
	if (active->corrupt_divider_after_write)
		map->divider ^= 0x100;
	if (active->read_error_after_write) {
		map->read_error = active->read_error_after_write;
		map->fail_offset = 0x54;
	}
	return active->write_error;
}

int regmap_write(struct regmap *map, unsigned int offset, unsigned int value)
{
	active->writes++;
	assert(offset == 0x54 || offset == 0x38);
	if (offset == 0x54)
		map->selection = value;
	else
		map->divider = value;
	return 0;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	active->registration_calls++;
	if (active->registration_error &&
	    (!active->fail_registration_call ||
	     active->registration_calls == active->fail_registration_call))
		return active->registration_error;
	active->provider_node = dev->of_node;
	assert(hw->init->num_parents == 1);
	assert(strcmp(hw->init->parent_data[0].fw_name, "source") == 0);
	if (strcmp(hw->init->name, "sc9820e-cpu") == 0) {
		assert(!active->clock);
		active->clock = hw;
		active->init = *hw->init;
	} else {
		assert(strcmp(hw->init->name, "sc9820e-twpll") == 0);
		assert(!active->twpll_clock);
		active->twpll_clock = hw;
		active->twpll_init = *hw->init;
	}
	return 0;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data)
{
	struct of_phandle_args args = { .args_count = 1 };

	(void)dev;
	if (active->provider_error)
		return active->provider_error;
	assert(active->clock && active->twpll_clock);
	assert(get(&args, data) == active->clock);
	args.args[0] = 1;
	assert(get(&args, data) == active->twpll_clock);
	return 0;
}

struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data)
{
	struct clk_hw_onecell_data *clocks = data;

	assert(args->args_count == 1);
	if (args->args[0] >= clocks->num)
		return ERR_PTR(-EINVAL);
	return clocks->hws[args->args[0]];
}

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	size_t index;

	assert(type == IORESOURCE_MEM);
	for (index = 0; index < ARRAY_SIZE(pdev->resources); index++)
		if (strcmp(pdev->resources[index].name, name) == 0)
			return &pdev->resources[index];
	return NULL;
}

unsigned long resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name)
{
	(void)pdev;
	if (active->map_error)
		return ERR_PTR(active->map_error);
	if (strcmp(name, "mpll") == 0)
		return active->mpll;
	assert(strcmp(name, "twpll") == 0);
	return active->twpll;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

struct clk_hw *clk_hw_get_parent(const struct clk_hw *hw)
{
	assert(hw == active->clock || hw == active->twpll_clock);
	return &active->reference;
}

unsigned long clk_hw_get_rate(const struct clk_hw *hw)
{
	assert(hw == &active->reference);
	return active->reference_hz;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data)
{
	(void)dev;
	assert(!active->cleanup);
	if (active->action_error) {
		action(data);
		return active->action_error;
	}
	active->cleanup = action;
	active->cleanup_data = data;
	return 0;
}

void mutex_init(struct mutex *lock)
{
	lock->held = false;
}

void mutex_lock(struct mutex *lock)
{
	assert(!lock->held);
	lock->held = true;
}

void mutex_unlock(struct mutex *lock)
{
	assert(lock->held);
	lock->held = false;
}

void cpumask_set_cpu(unsigned int cpu, struct cpumask *mask)
{
	mask->bits |= 1U << cpu;
}

bool cpumask_test_cpu(unsigned int cpu, const struct cpumask *mask)
{
	return mask->bits & (1U << cpu);
}

unsigned int cpumask_weight(const struct cpumask *mask)
{
	return __builtin_popcount(mask->bits);
}

void cpumask_copy(struct cpumask *destination, const struct cpumask *source)
{
	*destination = *source;
}

struct device *get_cpu_device(unsigned int cpu)
{
	assert(cpu < 2);
	active->cpu_nodes[cpu].clock_provider =
		(active->domain_members & (1U << cpu)) ? active->provider_node :
							 NULL;
	active->cpu_nodes[cpu].clock_output = active->cpu_clock_outputs[cpu];
	active->cpu_nodes[cpu].clock_args_count =
		active->cpu_clock_args_count[cpu];
	active->cpu_nodes[cpu].clock_error = active->cpu_clock_errors[cpu];
	active->cpu_devices[cpu].of_node = &active->cpu_nodes[cpu];
	return &active->cpu_devices[cpu];
}

int of_parse_phandle_with_args(const struct device_node *node,
			       const char *list_name, const char *cells_name,
			       int index, struct of_phandle_args *out_args)
{
	assert(strcmp(list_name, "clocks") == 0 && index == 0);
	assert(strcmp(cells_name, "#clock-cells") == 0);
	if (node->clock_error)
		return node->clock_error;
	if (!node->clock_provider)
		return -ENOENT;
	*out_args = (struct of_phandle_args){
		.np = node->clock_provider,
		.args_count = node->clock_args_count,
		.args = { node->clock_output },
	};
	return 0;
}

void of_node_put(struct device_node *node)
{
	(void)node;
}

void *cpufreq_get_driver_data(void)
{
	return active->cpufreq ? active->cpufreq->driver_data : NULL;
}

int cpufreq_register_driver(struct cpufreq_driver *driver)
{
	assert(!active->cpufreq);
	if (active->cpufreq_error)
		return active->cpufreq_error;
	active->cpufreq = driver;
	return 0;
}

void cpufreq_unregister_driver(struct cpufreq_driver *driver)
{
	assert(driver == active->cpufreq);
	active->cpufreq = NULL;
	active->unregisters++;
}

int cpufreq_generic_frequency_table_verify(struct cpufreq_policy_data *policy)
{
	(void)policy;
	return 0;
}

static void release_fixture(struct fixture *fixture)
{
	unsigned int index;

	if (fixture->cleanup)
		fixture->cleanup(fixture->cleanup_data);
	assert(!fixture->cpufreq);
	for (index = 0; index < fixture->allocation_count; index++)
		free(fixture->allocations[index]);
}

static struct platform_device device(struct device_node *node)
{
	struct platform_device pdev = {
		.dev.of_node = node,
		.resources = {
			{ .start = 0x403f0000, .end = 0x403f0007, .name = "mpll" },
			{ .start = 0x403c000c, .end = 0x403c0013, .name = "twpll" },
		},
	};

	return pdev;
}

struct rate_case {
	const char *name;
	u32 selection;
	u32 divider;
	u32 mpll[2];
	u32 twpll[2];
	unsigned long expected_hz;
};

static void check_rates(void)
{
	static const struct rate_case cases[] = {
		{
			.name = "oscillator",
			.expected_hz = 26000000,
		},
		{
			.name = "oscillator-divided-by-eight",
			.divider = 0x70,
			.expected_hz = 3250000,
		},
		{
			.name = "mpll-fractional-mode-whole-rate",
			.selection = 3,
			.mpll = { 0x45, 0x19000000 },
			.expected_hz = 1300000000,
		},
		{
			.name = "mpll-fractional-half-step",
			.selection = 3,
			.mpll = { 0x45, 0x19400000 },
			.expected_hz = 1313000000,
		},
		{
			.name = "sdm-disabled-ignores-fraction",
			.selection = 3,
			.mpll = { 0x01, 0x19400000 },
			.expected_hz = 1300000000,
		},
		{
			.name = "integer-mode-uses-n",
			.selection = 3,
			.mpll = { 0x500, 0xffffffff },
			.expected_hz = 1300000000,
		},
		{
			.name = "twpll-divided-by-three",
			.selection = 1,
			.twpll = { 0x85, 0x18000000 },
			.expected_hz = 416000000,
		},
		{
			.name = "twpll-divided-by-two",
			.selection = 2,
			.twpll = { 0x85, 0x18000000 },
			.expected_hz = 624000000,
		},
		{
			.name = "mcu-divider-rounds-up",
			.selection = 3,
			.divider = 0x20,
			.mpll = { 0x45, 0x19000000 },
			.expected_hz = 433333334,
		},
		{
			.name = "fraction-one-lsb",
			.selection = 3,
			.mpll = { 0x45, 0x19000001 },
			.expected_hz = 1300000003,
		},
		{
			.name = "fraction-near-next-integer",
			.selection = 3,
			.mpll = { 0x45, 0x197fffff },
			.expected_hz = 1325999997,
		},
		{
			.name = "twpll-factor-floors-before-mcu-division",
			.selection = 1,
			.divider = 0x10,
			.twpll = { 0x85, 0x1d900000 },
			.expected_hz = 256208333,
		},
		{
			.name = "unrelated-fields-ignored",
			.selection = 0xffffff83,
			.divider = 0xffffff8f,
			.mpll = { 0x45, 0x19000000 },
			.expected_hz = 1300000000,
		},
		{
			.name = "empty-pll-configuration",
			.selection = 3,
		},
		{
			.name = "unrepresentable-target-rate",
			.selection = 3,
			.mpll = { 0x7ff00, 0 },
		},
	};
	struct fixture fixture = {};
	struct device_node node = { .regmap = &fixture.ap_ahb };
	struct platform_device pdev = device(&node);
	const struct clk_ops *ops;
	size_t index;

	active = &fixture;
	assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
	assert(strcmp(fixture.init.name, "sc9820e-cpu") == 0);
	assert(fixture.init.flags & CLK_GET_RATE_NOCACHE);
	ops = fixture.init.ops;
	assert(ops->recalc_rate);
	assert(!ops->set_rate && !ops->set_parent && !ops->prepare &&
	       !ops->unprepare && !ops->enable && !ops->disable);

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		const struct rate_case *test = &cases[index];
		unsigned long actual_hz;

		fixture.ap_ahb.selection = test->selection;
		fixture.ap_ahb.divider = test->divider;
		memcpy(fixture.mpll, test->mpll, sizeof(fixture.mpll));
		memcpy(fixture.twpll, test->twpll, sizeof(fixture.twpll));
		actual_hz = ops->recalc_rate(fixture.clock, 26000000);
		if (actual_hz != test->expected_hz) {
			fprintf(stderr, "%s: expected %lu Hz, got %lu Hz\n",
				test->name, test->expected_hz, actual_hz);
			abort();
		}
		assert(fixture.ap_ahb.selection == test->selection);
		assert(fixture.ap_ahb.divider == test->divider);
		assert(memcmp(fixture.mpll, test->mpll, sizeof(fixture.mpll)) ==
		       0);
		assert(memcmp(fixture.twpll, test->twpll,
			      sizeof(fixture.twpll)) == 0);
		assert(fixture.writes == 0);
	}
	fixture.ap_ahb.selection = 3;
	fixture.ap_ahb.divider = 0;
	fixture.mpll[0] = 0x45;
	fixture.mpll[1] = 0x19000000;
	assert(ops->recalc_rate(fixture.clock, 26000000) == 1300000000);
	assert(ops->recalc_rate(fixture.clock, 25000000) == 0);

	fixture.ap_ahb.read_error = -EIO;
	fixture.ap_ahb.fail_offset = 0x54;
	assert(ops->recalc_rate(fixture.clock, 26000000) == 0);
	fixture.ap_ahb.fail_offset = 0x38;
	assert(ops->recalc_rate(fixture.clock, 26000000) == 0);
	assert(fixture.writes == 0);
	release_fixture(&fixture);
}

static void check_probe_errors(void)
{
	static const struct {
		int lookup_error;
		int map_error;
		int registration_error;
		unsigned int fail_registration_call;
		unsigned int fail_allocation_call;
		int provider_error;
		int invalid_resource;
		int expected;
	} cases[] = {
		{ .lookup_error = -EIO,
		  .invalid_resource = -1,
		  .expected = -EIO },
		{ .map_error = -EIO, .invalid_resource = -1, .expected = -EIO },
		{ .registration_error = -EIO,
		  .invalid_resource = -1,
		  .expected = -EIO },
		{ .registration_error = -EIO,
		  .fail_registration_call = 2,
		  .invalid_resource = -1,
		  .expected = -EIO },
		{ .fail_allocation_call = 1,
		  .invalid_resource = -1,
		  .expected = -ENOMEM },
		{ .fail_allocation_call = 2,
		  .invalid_resource = -1,
		  .expected = -ENOMEM },
		{ .provider_error = -EIO,
		  .invalid_resource = -1,
		  .expected = -EIO },
		{ .invalid_resource = 0, .expected = -EINVAL },
		{ .invalid_resource = 1, .expected = -EINVAL },
	};
	size_t index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		struct fixture fixture = {};
		struct device_node node = { .regmap = &fixture.ap_ahb };
		struct platform_device pdev = device(&node);

		active = &fixture;
		fixture.lookup_error = cases[index].lookup_error;
		fixture.map_error = cases[index].map_error;
		fixture.registration_error = cases[index].registration_error;
		fixture.fail_registration_call =
			cases[index].fail_registration_call;
		fixture.fail_allocation_call =
			cases[index].fail_allocation_call;
		fixture.provider_error = cases[index].provider_error;
		if (cases[index].invalid_resource >= 0)
			pdev.resources[cases[index].invalid_resource].end--;
		assert(host_sc9820e_cpu_clock_driver->probe(&pdev) ==
		       cases[index].expected);
		assert(fixture.writes == 0);
		release_fixture(&fixture);
	}
}

static void inherited_fixture(struct fixture *fixture)
{
	*fixture = (struct fixture){
		.ap_ahb = { .selection = 0xa5000003, .divider = 0x80010101 },
		.mpll = { 0x26a5, 0x19000000 },
		.twpll = { 0x3ba5, 0x1d89d89d },
		.reference_hz = 26000000,
		.domain_members = 3,
		.cpu_clock_args_count = { 1, 1 },
	};
	active = fixture;
}

static struct cpufreq_policy parent_policy(struct fixture *fixture,
					   struct cpumask *cpus)
{
	struct cpufreq_policy policy = { .cpu = 0, .cpus = cpus };

	assert(fixture->cpufreq);
	assert(fixture->cpufreq->init(&policy) == 0);
	assert(cpus->bits == 3);
	assert(policy.cur == 1300000);
	assert(policy.freq_table[0].frequency == 768000);
	assert(policy.freq_table[1].frequency == 1300000);
	assert(policy.freq_table[2].frequency == CPUFREQ_TABLE_END);
	assert(fixture->cpufreq->flags & CPUFREQ_CONST_LOOPS);
	assert(policy.cpuinfo.transition_latency ==
	       CPUFREQ_DEFAULT_TRANSITION_LATENCY_NS);
	assert(policy.dvfs_possible_from_any_cpu);
	return policy;
}

static void check_parent_transitions(void)
{
	struct fixture fixture;
	struct device_node node = { .regmap = &fixture.ap_ahb };
	struct platform_device pdev = device(&node);
	struct cpufreq_policy policy;
	struct cpumask cpus = {};
	const struct clk_ops *ops;

	inherited_fixture(&fixture);
	assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
	policy = parent_policy(&fixture, &cpus);
	ops = fixture.init.ops;
	assert(!ops->set_rate && !ops->set_parent);
	assert(fixture.cpufreq->get(0) == 1300000);
	assert(fixture.cpufreq->get(1) == 1300000);
	assert(fixture.cpufreq->get(2) == 0);
	assert(fixture.cpufreq->target_index(&policy, 0) == 0);
	assert(fixture.ap_ahb.selection == 0xa5000002);
	assert(fixture.cpufreq->get(0) == 768000);
	assert(fixture.cpufreq->get(1) == 768000);
	assert(ops->recalc_rate(fixture.clock, 26000000) == 767999999);
	assert(fixture.writes == 1);
	assert(fixture.cpufreq->target_index(&policy, 0) == 0);
	assert(fixture.writes == 1);
	assert(fixture.cpufreq->target_index(&policy, 1) == 0);
	assert(fixture.ap_ahb.selection == 0xa5000003);
	assert(ops->recalc_rate(fixture.clock, 26000000) == 1300000000);
	assert(fixture.writes == 2);
	assert(fixture.cpufreq->target_index(&policy, 2) == -EINVAL);
	assert(fixture.writes == 2);
	assert(fixture.ap_ahb.divider == 0x80010101);
	assert(fixture.mpll[0] == 0x26a5 && fixture.mpll[1] == 0x19000000);
	assert(fixture.twpll[0] == 0x3ba5 && fixture.twpll[1] == 0x1d89d89d);
	release_fixture(&fixture);
	assert(fixture.unregisters == 1 && fixture.writes == 2);
}

static void check_inherited_parent_rates(void)
{
	struct fixture fixture;
	struct device_node node = { .regmap = &fixture.ap_ahb };
	struct platform_device pdev = device(&node);
	struct cpumask cpus = {};
	struct cpufreq_policy policy = { .cpu = 1, .cpus = &cpus };

	inherited_fixture(&fixture);
	fixture.mpll[0] = 5;
	fixture.mpll[1] = 0x16800000;
	fixture.twpll[0] = 5;
	fixture.twpll[1] = 0x18000000;
	assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
	assert(fixture.cpufreq->init(&policy) == 0);
	assert(cpus.bits == 3 && policy.cur == 1170000);
	assert(policy.freq_table[0].frequency == 624000);
	assert(policy.freq_table[1].frequency == 1170000);
	assert(fixture.cpufreq->target_index(&policy, 0) == 0);
	assert(fixture.cpufreq->get(1) == 624000);
	assert(fixture.init.ops->recalc_rate(fixture.clock, 26000000) ==
	       624000000);
	assert(fixture.cpufreq->target_index(&policy, 1) == 0);
	assert(fixture.cpufreq->get(0) == 1170000);
	assert(fixture.mpll[0] == 5 && fixture.mpll[1] == 0x16800000);
	assert(fixture.twpll[0] == 5 && fixture.twpll[1] == 0x18000000);
	assert(fixture.writes == 2);
	release_fixture(&fixture);
}

static void check_parent_unknown_state(void)
{
	static const struct {
		unsigned int word;
		u32 mask;
	} cases[] = {
		{ 0, 3 },	{ 0, 0x80 }, { 1, 0x10 }, { 1, 0x100 },
		{ 1, 0x10000 }, { 1, 1 },    { 2, 1 },	  { 3, 1 },
		{ 4, 1 },	{ 5, 1 },
	};
	size_t index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		struct fixture fixture;
		struct device_node node = { .regmap = &fixture.ap_ahb };
		struct platform_device pdev = device(&node);
		struct cpufreq_policy policy;
		struct cpumask cpus = {};
		u32 *words[] = {
			&fixture.ap_ahb.selection, &fixture.ap_ahb.divider,
			&fixture.mpll[0],	   &fixture.mpll[1],
			&fixture.twpll[0],	   &fixture.twpll[1]
		};
		u32 changed;

		inherited_fixture(&fixture);
		assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
		policy = parent_policy(&fixture, &cpus);
		*words[cases[index].word] ^= cases[index].mask;
		changed = *words[cases[index].word];
		assert(fixture.cpufreq->target_index(&policy, 0) == -EIO);
		assert(fixture.cpufreq->get(0) == 0);
		assert(fixture.cpufreq->init(&policy) == -EIO);
		assert(fixture.cpufreq->target_index(&policy, 1) == -EIO);
		assert(*words[cases[index].word] == changed);
		assert(fixture.writes == 0);
		release_fixture(&fixture);
	}
}

static void check_parent_errors(void)
{
	unsigned int scenario;

	for (scenario = 0; scenario < 7; scenario++) {
		struct fixture fixture;
		struct device_node node = { .regmap = &fixture.ap_ahb };
		struct platform_device pdev = device(&node);
		struct cpufreq_policy policy;
		struct cpumask cpus = {};
		int expected = -EIO;

		inherited_fixture(&fixture);
		assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
		policy = parent_policy(&fixture, &cpus);
		if (scenario < 2) {
			fixture.ap_ahb.read_error = -EIO;
			fixture.ap_ahb.fail_offset = scenario ? 0x38 : 0x54;
		} else if (scenario < 4) {
			fixture.write_error = -EIO;
			fixture.apply_failed_write = scenario == 3;
		} else if (scenario == 4) {
			fixture.suppress_write = true;
		} else if (scenario == 5) {
			fixture.read_error_after_write = -ETIMEDOUT;
			expected = -ETIMEDOUT;
		} else {
			fixture.corrupt_divider_after_write = true;
		}
		assert(fixture.cpufreq->target_index(&policy, 0) == expected);
		assert(fixture.writes == (scenario < 2 ? 0 : 1));
		if (scenario < 2 || scenario == 5 || scenario == 6)
			assert(fixture.cpufreq->get(0) == 0);
		if (scenario < 3 || scenario == 4)
			assert(fixture.ap_ahb.selection == 0xa5000003);
		else
			assert(fixture.ap_ahb.selection == 0xa5000002);
		if (scenario == 5 || scenario == 6) {
			assert(fixture.cpufreq->target_index(&policy, 1) != 0);
			assert(fixture.writes == 1);
		}
		if (scenario != 6) {
			fixture.ap_ahb.read_error = 0;
			fixture.read_error_after_write = 0;
			fixture.write_error = 0;
			fixture.suppress_write = false;
			assert(fixture.cpufreq->target_index(&policy, 1) == 0);
			assert(fixture.ap_ahb.selection == 0xa5000003);
			assert(fixture.writes ==
			       (scenario < 2		       ? 0 :
				scenario == 3 || scenario == 5 ? 2 :
								 1));
		}
		release_fixture(&fixture);
	}
}

static void check_parent_probe_errors(void)
{
	unsigned int scenario;

	for (scenario = 0; scenario < 11; scenario++) {
		struct fixture fixture;
		struct device_node node = { .regmap = &fixture.ap_ahb };
		struct platform_device pdev = device(&node);
		int expected = -EINVAL;

		inherited_fixture(&fixture);
		switch (scenario) {
		case 0:
			fixture.ap_ahb.selection = 2;
			break;
		case 1:
			fixture.ap_ahb.divider |= 0x10;
			break;
		case 2:
			fixture.twpll[0] = fixture.twpll[1] = 0;
			break;
		case 3:
			fixture.twpll[0] = 0xa00;
			fixture.twpll[1] = 0;
			break;
		case 4:
			fixture.mpll[0] = 0x7ff00;
			break;
		case 5:
			fixture.reference_hz = 25000000;
			break;
		case 6:
			fixture.domain_members = 1;
			break;
		case 7:
			fixture.cpufreq_error = expected = -EAGAIN;
			break;
		case 8:
			fixture.action_error = expected = -ENOMEM;
			break;
		case 9:
			fixture.mutate_selection_read = 2;
			expected = -EIO;
			break;
		case 10:
			fixture.mpll[0] = 5;
			fixture.mpll[1] = 0x0c000001;
			fixture.twpll[0] = 5;
			fixture.twpll[1] = 0x18000000;
			break;
		}
		assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == expected);
		assert(!fixture.cpufreq && fixture.writes == 0);
		assert(fixture.unregisters == (scenario == 8 ? 1 : 0));
		release_fixture(&fixture);
	}
}

static void check_twpll_output(void)
{
	static const struct {
		u32 controls[2];
		unsigned long expected_hz;
	} cases[] = {
		{ { 0x3ba5, 0x1d89d89d }, 1535999998 },
		{ { 0x45, 0x19000000 }, 1300000000 },
		{ { 0x500, 0xffffffff }, 1300000000 },
		{ { 0x7ff00, 0 }, 0 },
		{ { 0, 0 }, 0 },
	};
	struct fixture fixture;
	struct device_node node = { .regmap = &fixture.ap_ahb };
	struct platform_device pdev = device(&node);
	struct cpufreq_policy policy;
	struct cpumask cpus = {};
	const struct clk_ops *ops;
	size_t index;

	inherited_fixture(&fixture);
	assert(host_sc9820e_cpu_clock_driver->probe(&pdev) == 0);
	assert(fixture.twpll_clock != fixture.clock);
	assert(fixture.twpll_init.flags & CLK_GET_RATE_NOCACHE);
	ops = fixture.twpll_init.ops;
	assert(ops->recalc_rate);
	assert(!ops->set_rate && !ops->set_parent && !ops->prepare &&
	       !ops->unprepare && !ops->enable && !ops->disable);
	assert(ops->recalc_rate(fixture.twpll_clock, 26000000) == 1535999998);
	assert(fixture.writes == 0);
	policy = parent_policy(&fixture, &cpus);
	assert(fixture.cpufreq->target_index(&policy, 0) == 0);
	assert(ops->recalc_rate(fixture.twpll_clock, 26000000) == 1535999998);
	assert(fixture.cpufreq->target_index(&policy, 1) == 0);
	assert(ops->recalc_rate(fixture.twpll_clock, 26000000) == 1535999998);
	assert(fixture.writes == 2);

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		memcpy(fixture.twpll, cases[index].controls,
		       sizeof(fixture.twpll));
		assert(ops->recalc_rate(fixture.twpll_clock, 26000000) ==
		       cases[index].expected_hz);
		assert(!memcmp(fixture.twpll, cases[index].controls,
			       sizeof(fixture.twpll)));
		assert(fixture.writes == 2);
	}
	assert(ops->recalc_rate(fixture.twpll_clock, 25000000) == 0);
	assert(fixture.writes == 2);
	release_fixture(&fixture);
	assert(fixture.unregisters == 1);
}

static void check_cpu_output_selection(void)
{
	static const struct {
		unsigned int output0;
		unsigned int output1;
		int args_count1;
		int error1;
		int expected;
	} cases[] = {
		{ 0, 1, 1, 0, -EINVAL },
		{ 1, 0, 1, 0, 0 },
		{ 0, 0, 0, 0, -EINVAL },
		{ 0, 0, 1, -EINVAL, -EINVAL },
	};
	size_t index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		struct fixture fixture;
		struct device_node node = { .regmap = &fixture.ap_ahb };
		struct platform_device pdev = device(&node);

		inherited_fixture(&fixture);
		fixture.cpu_clock_outputs[0] = cases[index].output0;
		fixture.cpu_clock_outputs[1] = cases[index].output1;
		fixture.cpu_clock_args_count[1] = cases[index].args_count1;
		fixture.cpu_clock_errors[1] = cases[index].error1;
		assert(host_sc9820e_cpu_clock_driver->probe(&pdev) ==
		       cases[index].expected);
		assert(!fixture.cpufreq && fixture.writes == 0);
		assert(fixture.ap_ahb.selection == 0xa5000003);
		assert(fixture.twpll[0] == 0x3ba5 &&
		       fixture.twpll[1] == 0x1d89d89d);
		release_fixture(&fixture);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (strcmp(argv[1], "rates") == 0)
		check_rates();
	else if (strcmp(argv[1], "probe-errors") == 0)
		check_probe_errors();
	else if (strcmp(argv[1], "parent-transitions") == 0) {
		check_parent_transitions();
		check_inherited_parent_rates();
	} else if (strcmp(argv[1], "parent-unknown-state") == 0)
		check_parent_unknown_state();
	else if (strcmp(argv[1], "parent-errors") == 0)
		check_parent_errors();
	else if (strcmp(argv[1], "twpll-output") == 0)
		check_twpll_output();
	else if (strcmp(argv[1], "cpu-output-selection") == 0)
		check_cpu_output_selection();
	else {
		assert(strcmp(argv[1], "parent-probe-errors") == 0);
		check_parent_probe_errors();
	}
	return 0;
}
