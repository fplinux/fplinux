// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <display-test-kernel.h>

#include "ums9117-drm-internal.h"

struct named_mapping {
	const char *name;
	struct resource resource;
	u32 words[0x120 / 4];
	unsigned int map_calls;
};

struct regmap {
	u32 words[0x1000 / 4];
};

static struct named_mapping mappings[] = {
	{ "lcdc", { 0x1000, 0x111f }, {}, 0 },
	{ "ap-ahb-gate-set", { 0x2000, 0x200f }, {}, 0 },
	{ "ap-ahb-reset-set", { 0x2100, 0x2103 }, {}, 0 },
	{ "ap-ahb-reset-clear", { 0x2200, 0x2207 }, {}, 0 },
	{ "pinmux", { 0x3000, 0x3003 }, {}, 0 },
	{ "pinconf", { 0x3100, 0x3103 }, {}, 0 },
	{ "pinmux-data", { 0x3200, 0x3203 }, {}, 0 },
	{ "pinconf-data", { 0x3300, 0x3303 }, {}, 0 },
};

static const struct ums9117_drm_profile profile = {
	.name = "host-lcm",
	.transport = UMS9117_DRM_TRANSPORT_LCM_DBI,
	.wled_backlight_name = "host-backlight",
};
static struct platform_device device;
static struct regmap aon;
static struct regmap pmic;
static const char *failed_mapping;
static u32 pin_allocations[8];
static unsigned int pin_allocation_count;
static unsigned int invalid_mmio;
static unsigned int transport_initializations;
static unsigned int transport_enables;
static unsigned int irq_requests;

static struct named_mapping *mapping_named(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mappings); i++)
		if (!strcmp(name, mappings[i].name))
			return &mappings[i];
	return NULL;
}

static u32 *mapped_word(const void *address)
{
	uintptr_t value = (uintptr_t)address;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mappings); i++) {
		uintptr_t base = (uintptr_t)mappings[i].words;
		size_t size = resource_size(&mappings[i].resource);

		if (value >= base && value - base < size &&
		    !((value - base) % 4))
			return &mappings[i].words[(value - base) / 4];
	}
	invalid_mmio++;
	return NULL;
}

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	struct named_mapping *mapping = mapping_named(name);

	assert(pdev == &device);
	assert(type == 0x00000200);
	return mapping ? &mapping->resource : NULL;
}

void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name)
{
	struct named_mapping *mapping = mapping_named(name);

	assert(pdev == &device && mapping);
	return mapping->words;
}

void *devm_ioremap(struct device *dev, resource_size_t start,
		   resource_size_t size)
{
	unsigned int i;

	assert(dev == &device.dev);
	for (i = 0; i < ARRAY_SIZE(mappings); i++) {
		struct named_mapping *mapping = &mappings[i];

		if (mapping->resource.start != start)
			continue;
		assert(size == resource_size(&mapping->resource));
		mapping->map_calls++;
		return failed_mapping &&
				       !strcmp(failed_mapping, mapping->name) ?
			       NULL :
			       mapping->words;
	}
	assert(!"unexpected shared mapping");
	return NULL;
}

u32 readl(const void *address)
{
	u32 *word = mapped_word(address);

	return word ? *word : 0;
}

void writel(u32 value, void *address)
{
	u32 *word = mapped_word(address);

	if (word)
		*word = value;
}

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	if (!strcmp(name, "sprd,aon-apb"))
		return &aon;
	assert(!strcmp(name, "sprd,pmic"));
	return &pmic;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	assert(!(reg % 4) && reg < sizeof(map->words));
	*value = map->words[reg / 4];
	return 0;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	assert(!(reg % 4) && reg < sizeof(map->words));
	map->words[reg / 4] = value;
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	unsigned int previous;

	regmap_read(map, reg, &previous);
	return regmap_write(map, reg, (previous & ~mask) | (value & mask));
}

int of_property_count_u32_elems(const struct device_node *node,
				const char *name)
{
	return 1;
}

int of_property_read_u32_array(const struct device_node *node, const char *name,
			       u32 *values, size_t count)
{
	if (!strcmp(name, "sprd,wled-current-levels")) {
		assert(count == 4);
		values[0] = 16;
		values[1] = 16;
		values[2] = 16;
		values[3] = 16;
		return 0;
	}
	assert(count == 1);
	values[0] = strstr(name, "-values") ? 0x55 : 0;
	return 0;
}

struct property *of_find_property(const struct device_node *node,
				  const char *name, int *length)
{
	assert(!strcmp(name, "sprd,wled-default-level"));
	return NULL;
}

int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *value)
{
	assert(!"unexpected scalar property");
	return -22;
}

void *devm_kmalloc_array(struct device *dev, size_t count, size_t size,
			 unsigned int flags)
{
	assert(dev == &device.dev && count == 1 && size == 4);
	assert(pin_allocation_count < ARRAY_SIZE(pin_allocations));
	return &pin_allocations[pin_allocation_count++];
}

int ums9117_drm_lcm_init_transport(struct ums9117_drm *udrm,
				   struct platform_device *pdev)
{
	transport_initializations++;
	return 0;
}

int ums9117_drm_lcm_enable_transport(struct ums9117_drm *udrm)
{
	transport_enables++;
	return 0;
}

int platform_get_irq(struct platform_device *pdev, unsigned int index)
{
	assert(pdev == &device && index == 0);
	return 7;
}

int devm_request_irq(struct device *dev, int irq,
		     irqreturn_t (*handler)(int, void *), unsigned long flags,
		     const char *name, void *data)
{
	assert(dev == &device.dev && irq == 7 && handler && data);
	irq_requests++;
	return 0;
}

const char *dev_name(const struct device *dev)
{
	return "host-display";
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
}

/* These boundaries must stay outside the initialization scenarios. */
int ums9117_drm_lcm_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			size_t length)
{
	assert(!"unexpected panel command");
	return -22;
}

void msleep(unsigned int delay)
{
	assert(!"unexpected millisecond sleep");
}

void *bl_get_data(struct backlight_device *backlight)
{
	assert(!"unexpected backlight callback");
	return NULL;
}

int backlight_get_brightness(struct backlight_device *backlight)
{
	assert(!"unexpected backlight callback");
	return 0;
}

int main(int argc, char *argv[])
{
	struct ums9117_drm display = { .profile = &profile };
	int expected;
	int ret;

	assert(argc == 2);
	failed_mapping = strcmp(argv[1], "success") ? argv[1] : NULL;
	assert(!failed_mapping || mapping_named(failed_mapping));
	expected = failed_mapping ? -12 : 0;
	ret = ums9117_drm_display_init(&display, &device);
	if (ret != expected) {
		fprintf(stderr, "%s: display_init returned %d, expected %d\n",
			argv[1], ret, expected);
		return 1;
	}
	assert(invalid_mmio == 0);
	if (failed_mapping) {
		assert(transport_initializations == 0);
		assert(transport_enables == 0);
		assert(irq_requests == 0);
		assert(mapping_named(failed_mapping)->map_calls == 1);
	} else {
		assert(transport_initializations == 1);
		assert(transport_enables == 1);
		assert(irq_requests == 1);
		assert(mappings[1].words[0] == 0x1800);
		assert(mappings[2].words[0] == 2);
		assert(mappings[3].words[0] == 2);
		assert(pin_allocation_count == 8);
		assert(display.wled_known && !display.wled_on);
	}
	puts("DRM display resource checks passed");
	return 0;
}
