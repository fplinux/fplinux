// SPDX-License-Identifier: GPL-2.0-only
#include "gpio-compat/gpio-kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Managed resources model subsystem teardown; driver callbacks remain real. */
struct managed_resource {
	void (*release)(void *data);
	void *data;
};

enum cleanup_event {
	IRQ_RELEASE,
	GPIO_RELEASE,
	MMIO_UNMAP,
	CLOCK_DISABLE,
};

struct regmap {
	bool enabled;
	unsigned int enables;
	unsigned int disables;
	int enable_error;
};

struct gpio_fixture {
	struct regmap regmap;
	struct clk clock;
	struct clk_init_data clock_init;
	struct device_node gate_node;
	struct platform_device gate_device;
	struct device *gpio_device;
	void *gate_allocation;
	struct managed_resource resources[6];
	size_t resource_count;
	enum cleanup_event events[4];
	size_t event_count;
	u32 registers[512];
	u32 ngpio;
	unsigned int maps;
	unsigned int accesses;
	unsigned int property_reads;
	unsigned int registrations;
	bool mapped;
	bool registered;
	bool clock_absent;
	int map_error;
	int registration_error;
};

static struct gpio_fixture *active;

static void assert_clock_available(void)
{
	assert(active->clock_absent || active->regmap.enabled);
}

static void record_event(enum cleanup_event event)
{
	assert(active->event_count < ARRAY_SIZE(active->events));
	active->events[active->event_count++] = event;
}

static void add_resource(void (*release)(void *), void *data)
{
	struct managed_resource *resource;

	assert(active->resource_count < ARRAY_SIZE(active->resources));
	resource = &active->resources[active->resource_count++];
	resource->release = release;
	resource->data = data;
}

static void release_resources(void)
{
	while (active->resource_count) {
		struct managed_resource *resource =
			&active->resources[--active->resource_count];

		resource->release(resource->data);
	}
}

const void *device_get_match_data(const struct device *dev)
{
	return dev->match_data;
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	void *allocation = calloc(1, size);

	(void)flags;
	assert(allocation);
	if (dev == active->gpio_device)
		add_resource(free, allocation);
	else {
		assert(!active->gate_allocation);
		active->gate_allocation = allocation;
	}
	return allocation;
}

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	assert(strcmp(name, "fplinux,aon-syscon") == 0);
	return node->regmap;
}

int regmap_write(struct regmap *regmap, unsigned int offset, unsigned int value)
{
	assert(value == 0x00000008);
	if (offset == 0x1000) {
		regmap->enables++;
		if (regmap->enable_error)
			return regmap->enable_error;
		assert(!regmap->enabled);
		regmap->enabled = true;
	} else {
		assert(offset == 0x2000);
		assert(regmap->enabled);
		assert(!active->mapped && !active->registered);
		regmap->disables++;
		regmap->enabled = false;
		record_event(CLOCK_DISABLE);
	}
	return 0;
}

int regmap_read(struct regmap *regmap, unsigned int offset, unsigned int *value)
{
	assert(offset == 0);
	*value = regmap->enabled ? 0x00000008 : 0;
	return 0;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	(void)dev;
	active->clock.hw = hw;
	active->clock_init = *hw->init;
	return 0;
}

struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *args, void *data)
{
	(void)args;
	return data;
}

int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data)
{
	struct of_phandle_args args = {};

	(void)dev;
	assert(get == of_clk_hw_simple_get);
	assert(get(&args, data) == active->clock.hw);
	return 0;
}

static void release_clock(void *data)
{
	struct clk *clock = data;

	active->clock_init.ops->disable(clock->hw);
}

struct clk *devm_clk_get_optional_enabled(struct device *dev, const char *name)
{
	int ret;

	assert(dev == active->gpio_device);
	assert(strcmp(name, "enable") == 0);
	if (active->clock_absent)
		return NULL;
	ret = active->clock_init.ops->enable(active->clock.hw);
	if (ret)
		return ERR_PTR(ret);
	add_resource(release_clock, &active->clock);
	return &active->clock;
}

int platform_get_irq(struct platform_device *pdev, unsigned int index)
{
	(void)pdev;
	assert(index == 0);
	return 7;
}

int device_property_read_u32(struct device *dev, const char *name, u32 *value)
{
	(void)dev;
	assert(strcmp(name, "ngpios") == 0);
	active->property_reads++;
	*value = active->ngpio;
	return 0;
}

const char *dev_name(const struct device *dev)
{
	(void)dev;
	return "gpio-host";
}

static void release_mapping(void *data)
{
	assert(data == active->registers);
	assert_clock_available();
	assert(!active->registered);
	active->mapped = false;
	record_event(MMIO_UNMAP);
}

void *devm_platform_ioremap_resource(struct platform_device *pdev,
				     unsigned int index)
{
	(void)pdev;
	assert(index == 0);
	assert_clock_available();
	active->maps++;
	if (active->map_error)
		return ERR_PTR(active->map_error);
	active->mapped = true;
	add_resource(release_mapping, active->registers);
	return active->registers;
}

u32 readl_relaxed(const void *address)
{
	assert_clock_available();
	assert(active->mapped);
	active->accesses++;
	return *(const u32 *)address;
}

void writel_relaxed(u32 value, void *address)
{
	assert_clock_available();
	assert(active->mapped);
	active->accesses++;
	*(u32 *)address = value;
}

void gpiochip_disable_irq(struct gpio_chip *chip, unsigned int offset)
{
	(void)chip;
	assert(offset == 0);
	assert_clock_available();
}

void gpiochip_enable_irq(struct gpio_chip *chip, unsigned int offset)
{
	(void)chip;
	assert(offset == 0);
	assert_clock_available();
}

static void release_gpio(void *data)
{
	struct gpio_chip *chip = data;

	assert_clock_available();
	chip->free(chip, 0);
	active->registered = false;
	record_event(GPIO_RELEASE);
}

static void release_irq(void *data)
{
	struct gpio_chip *chip = data;
	struct irq_data irq = { .chip = chip, .hwirq = 0 };

	assert_clock_available();
	chip->irq.chip->irq_mask(&irq);
	record_event(IRQ_RELEASE);
}

int devm_gpiochip_add_data(struct device *dev, struct gpio_chip *chip,
			   void *data)
{
	(void)dev;
	assert_clock_available();
	assert(active->mapped);
	assert(chip->ngpio == active->ngpio);
	assert(chip->irq.num_parents == 1 && *chip->irq.parents == 7);
	active->registrations++;
	if (active->registration_error)
		return active->registration_error;
	chip->data = data;
	assert(chip->request(chip, 0) == 0);
	active->registered = true;
	add_resource(release_gpio, chip);
	add_resource(release_irq, chip);
	return 0;
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
	assert(!"unexpected clock disable error");
}

static void probe_gate(struct gpio_fixture *fixture)
{
	const struct of_device_id *match =
		host_sc9820e_peripheral_gates_driver->driver.of_match_table;
	struct platform_device *device = &fixture->gate_device;

	for (; match->compatible; match++) {
		if (strcmp(match->compatible, "fplinux,sc9820e-gpio-gate") == 0)
			break;
	}
	assert(match->compatible);
	fixture->gate_node.regmap = &fixture->regmap;
	device->dev.of_node = &fixture->gate_node;
	device->dev.match_data = match->data;
	assert(host_sc9820e_peripheral_gates_driver->probe(device) == 0);
	assert(fixture->regmap.enables == 0);
}

static void check_cleanup(const struct gpio_fixture *fixture, int expected)
{
	assert(!fixture->mapped && !fixture->registered);
	assert(!fixture->regmap.enabled);
	if (fixture->clock_absent) {
		assert(fixture->regmap.enables == 0);
		assert(fixture->regmap.disables == 0);
	} else if (fixture->regmap.enable_error) {
		assert(fixture->regmap.enables == 1);
		assert(fixture->regmap.disables == 0);
		assert(fixture->maps == 0 && fixture->accesses == 0);
		assert(fixture->property_reads == 0 &&
		       fixture->registrations == 0);
		assert(fixture->event_count == 0);
		return;
	} else {
		assert(fixture->regmap.enables == 1);
		assert(fixture->regmap.disables == 1);
		assert(fixture->events[fixture->event_count - 1] ==
		       CLOCK_DISABLE);
	}
	if (!expected) {
		assert(fixture->events[0] == IRQ_RELEASE);
		assert(fixture->events[1] == GPIO_RELEASE);
		assert(fixture->events[2] == MMIO_UNMAP);
		assert(fixture->accesses >= 6);
	} else if (fixture->registration_error) {
		assert(fixture->events[0] == MMIO_UNMAP);
		assert(fixture->accesses == 0);
	} else {
		assert(fixture->accesses == 0 && fixture->registrations == 0);
	}
}

int main(int argc, char **argv)
{
	struct gpio_fixture fixture = { .ngpio = 32 };
	struct platform_device device = {};
	int expected = 0;

	assert(argc == 2);
	active = &fixture;
	fixture.gpio_device = &device.dev;
	if (strcmp(argv[1], "map-failure") == 0)
		expected = fixture.map_error = -ENXIO;
	else if (strcmp(argv[1], "gpio-failure") == 0)
		expected = fixture.registration_error = -EBUSY;
	else if (strcmp(argv[1], "enable-failure") == 0)
		expected = fixture.regmap.enable_error = -ETIMEDOUT;
	else if (strcmp(argv[1], "invalid-ngpio-zero") == 0) {
		fixture.ngpio = 0;
		expected = -EINVAL;
	} else if (strcmp(argv[1], "invalid-ngpio-large") == 0) {
		fixture.ngpio = 257;
		expected = -EINVAL;
	} else if (strcmp(argv[1], "optional-absent") == 0)
		fixture.clock_absent = true;
	else
		assert(strcmp(argv[1], "probe-success") == 0);
	probe_gate(&fixture);
	assert(host_sprd_gpio_driver->probe(&device) == expected);
	release_resources();
	check_cleanup(&fixture, expected);
	free(fixture.gate_allocation);
	puts("SC9820E GPIO host managed-lifetime cases passed");
	return 0;
}
