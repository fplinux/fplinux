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

struct backlight_device {
	void *data;
	const struct backlight_ops *ops;
	unsigned int brightness;
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
static const struct ums9117_drm_profile current_duty_profile = {
	.name = "host-current-duty",
	.transport = UMS9117_DRM_TRANSPORT_LCM_DBI,
	.wled_backlight_name = "host-backlight",
	.wled_duty = 0xffff,
};
static const struct ums9117_drm_profile white_pwm_profile = {
	.name = "host-white-pwm",
	.transport = UMS9117_DRM_TRANSPORT_LCM_DBI,
	.wled_backlight_name = "host-backlight",
	.wled_mode = UMS9117_DRM_WLED_WHITE_PWM,
};
static struct platform_device device;
static struct backlight_device backlight;
static struct regmap aon;
static struct regmap pmic;
static const char *failed_mapping;
static u32 pin_allocations[8];
static unsigned int pin_allocation_count;
static unsigned int invalid_mmio;
static unsigned int mmio_writes;
static unsigned int regmap_writes;
static unsigned int transport_initializations;
static unsigned int transport_enables;
static unsigned int irq_requests;
static unsigned int failed_pmic_register;
static bool white_pwm;
static bool backlight_test;

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

	mmio_writes++;
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
	regmap_writes++;
	if (map == &pmic && failed_pmic_register == reg)
		return -5;
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
		values[0] = white_pwm ? 0 : 16;
		values[1] = white_pwm ? 0 : 16;
		values[2] = white_pwm ? 0 : 16;
		values[3] = white_pwm ? 34 : 16;
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
	assert(backlight_test && command == 0x28 && !data && !length);
	return 0;
}

void msleep(unsigned int delay)
{
	assert(!"unexpected millisecond sleep");
}

void *bl_get_data(struct backlight_device *backlight)
{
	return backlight->data;
}

int backlight_get_brightness(struct backlight_device *backlight)
{
	return backlight->brightness;
}

struct backlight_device *
devm_backlight_device_register(struct device *dev, const char *name,
			       struct device *parent, void *data,
			       const struct backlight_ops *ops,
			       const struct backlight_properties *properties)
{
	assert(dev == &device.dev && parent == dev);
	assert(!strcmp(name, "host-backlight"));
	assert(properties->max_brightness == (white_pwm ? 255 : 16));
	assert(properties->brightness == properties->max_brightness);
	backlight.data = data;
	backlight.ops = ops;
	backlight.brightness = properties->brightness;
	return &backlight;
}

void synchronize_irq(int irq)
{
	assert(irq == 7);
}

static void check_backlight(struct ums9117_drm *display, bool fail_duty,
			    u16 expected_current_duty)
{
	static const unsigned int pwm_levels[] = { 1, 17, 255 };
	static const unsigned int current_levels[] = { 1, 9, 16 };
	const unsigned int *levels = white_pwm ? pwm_levels : current_levels;
	unsigned int i;

	assert(ums9117_drm_display_register_backlight(display, &device) == 0);
	display->state = UMS9117_DRM_PANEL_STATE_ACTIVE;
	if (fail_duty) {
		backlight.brightness = 17;
		assert(backlight.ops->update_status(&backlight) == 0);
		assert(pmic.words[0x180 / 4] & 0x1000);
		failed_pmic_register = 0x1cc;
		backlight.brightness = 255;
		assert(backlight.ops->update_status(&backlight) == -5);
		assert(display->state == UMS9117_DRM_PANEL_STATE_ERROR);
		assert(display->stats.wled_errors == 1);
		assert(display->wled_known && !display->wled_on);
		assert(!(pmic.words[0x180 / 4] & 0x1000));
		assert(backlight.ops->get_brightness(&backlight) == 0);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(pwm_levels); i++) {
		backlight.brightness = levels[i];
		assert(backlight.ops->update_status(&backlight) == 0);
		assert(backlight.ops->get_brightness(&backlight) == levels[i]);
		if (white_pwm) {
			assert(pmic.words[0x1b8 / 4] == 0xa5a5);
			assert(pmic.words[0x1bc / 4] == 0xa5a5);
			assert(pmic.words[0x1c0 / 4] == 0xa5a5);
			assert(pmic.words[0x1c4 / 4] == 0xa5a2);
			assert(pmic.words[0x1c8 / 4] == 0xa500);
			assert(pmic.words[0x1cc / 4] ==
			       (levels[i] << 8 | 0xff));
			assert(pmic.words[0x180 / 4] == 0xb5a5);
			assert(pmic.words[0x1d8 / 4] == 0xa5a5);
			assert(pmic.words[0xc10 / 4] == 0xb5a5);
		} else {
			assert(pmic.words[0x1b8 / 4] == (0xa580 | levels[i]));
			assert(pmic.words[0x1bc / 4] == (0xa580 | levels[i]));
			assert(pmic.words[0x1c0 / 4] == (0xa580 | levels[i]));
			assert(pmic.words[0x1c4 / 4] == (0xa580 | levels[i]));
			assert(pmic.words[0x1cc / 4] == expected_current_duty);
			assert(pmic.words[0x180 / 4] == 0xcccc);
			assert(pmic.words[0xc10 / 4] == 0xa5a5);
		}
	}
	backlight.brightness = 0;
	assert(backlight.ops->update_status(&backlight) == 0);
	assert(backlight.ops->get_brightness(&backlight) == 0);
	assert(display->wled_known && !display->wled_on);
	assert(pmic.words[0x180 / 4] == (white_pwm ? 0xa5a5 : 0));
	assert(pmic.words[0xc08 / 4] == 0xa7a5);
	assert(pmic.words[0xdec / 4] == 0xa5a0);
}

int main(int argc, char *argv[])
{
	struct ums9117_drm display = { .profile = &profile };
	int expected;
	int ret;

	assert(argc == 2);
	white_pwm = !strncmp(argv[1], "white-pwm", 9);
	backlight_test = white_pwm || !strcmp(argv[1], "current-backlight") ||
			 !strcmp(argv[1], "current-duty");
	if (white_pwm)
		display.profile = &white_pwm_profile;
	else if (!strcmp(argv[1], "current-duty"))
		display.profile = &current_duty_profile;
	if (backlight_test) {
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(pmic.words); i++)
			pmic.words[i] = 0xa5a5;
	}
	failed_mapping =
		strcmp(argv[1], "success") && !backlight_test ? argv[1] : NULL;
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
		assert(mmio_writes == 0);
		assert(regmap_writes == 0);
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
		if (backlight_test)
			check_backlight(
				&display, !strcmp(argv[1], "white-pwm-error"),
				!strcmp(argv[1], "current-duty") ? 0xffff : 0);
	}
	puts("DRM display resource checks passed");
	return 0;
}
