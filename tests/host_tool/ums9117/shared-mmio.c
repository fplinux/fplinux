// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <linux/soc/ums9117/mmio.h>

static struct resource control;
static u32 mapped_register;
struct mapping_record {
	struct device *device;
	resource_size_t start;
	resource_size_t size;
	unsigned int calls;
	bool fail;
};

static struct mapping_record mapper;

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	assert(pdev);
	assert(type == 0x00000200);
	if (!strcmp(name, "control"))
		return &control;
	return NULL;
}

void *devm_ioremap(struct device *dev, resource_size_t start,
		   resource_size_t size)
{
	mapper.device = dev;
	mapper.start = start;
	mapper.size = size;
	mapper.calls++;
	return mapper.fail ? NULL : &mapped_register;
}

static void reset_mapping(resource_size_t start, resource_size_t end)
{
	control.start = start;
	control.end = end;
	memset(&mapper, 0, sizeof(mapper));
}

static void check_minimum_size_mapping(void)
{
	struct platform_device device = {};
	void *mapping;

	reset_mapping(0x2000, 0x203f);
	mapping = ums9117_devm_ioremap_shared(&device, "control", 64);
	assert(mapping == &mapped_register);
	assert(mapper.device == &device.dev);
	assert(mapper.start == 0x2000);
	assert(mapper.size == 64);
	assert(mapper.calls == 1);
	reset_mapping(0x3000, 0x307f);
	mapping = ums9117_devm_ioremap_shared(&device, "control", 64);
	assert(mapping == &mapped_register);
	assert(mapper.start == 0x3000);
	assert(mapper.size == 128);
	reset_mapping(0x3000, 0x303e);
	mapping = ums9117_devm_ioremap_shared(&device, "control", 64);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	mapping = ums9117_devm_ioremap_shared(&device, "missing", 4);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	reset_mapping(0x2000, 0x203f);
	mapper.fail = true;
	mapping = ums9117_devm_ioremap_shared(&device, "control", 64);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -12);
	assert(mapper.device == &device.dev);
	assert(mapper.start == 0x2000);
	assert(mapper.size == 64);
	assert(mapper.calls == 1);
}

static void check_exact_register_mapping(void)
{
	struct platform_device device = {};
	void *mapping;

	reset_mapping(0x20e00000, 0x20e00003);
	mapping = ums9117_devm_ioremap_shared_register(&device, "control",
						       0x20e00000);
	assert(mapping == &mapped_register);
	assert(mapper.device == &device.dev);
	assert(mapper.start == 0x20e00000);
	assert(mapper.size == 4);
	assert(mapper.calls == 1);
	reset_mapping(0x20e00004, 0x20e00007);
	mapping = ums9117_devm_ioremap_shared_register(&device, "control",
						       0x20e00000);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	reset_mapping(0x20e00000, 0x20e00002);
	mapping = ums9117_devm_ioremap_shared_register(&device, "control",
						       0x20e00000);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	reset_mapping(0x20e00000, 0x20e00004);
	mapping = ums9117_devm_ioremap_shared_register(&device, "control",
						       0x20e00000);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	mapping = ums9117_devm_ioremap_shared_register(&device, "missing",
						       0x20e00000);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -22);
	assert(mapper.calls == 0);
	reset_mapping(0x20e00000, 0x20e00003);
	mapper.fail = true;
	mapping = ums9117_devm_ioremap_shared_register(&device, "control",
						       0x20e00000);
	assert(IS_ERR(mapping));
	assert(PTR_ERR(mapping) == -12);
	assert(mapper.device == &device.dev);
	assert(mapper.start == 0x20e00000);
	assert(mapper.size == 4);
	assert(mapper.calls == 1);
}

int main(void)
{
	check_minimum_size_mapping();
	check_exact_register_mapping();
	puts("Shared MMIO mapping checks passed");
	return 0;
}
