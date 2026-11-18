// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <linux/err.h>
#include <linux/regmap.h>

#include "host.h"
#include "ums9117-sc2720-codec.h"

/* Register storage and no-op time replace the host's absent analog hardware. */
static struct regmap registers;
static struct clk xtal;
static void (*release_action)(void *);
static void *allocation;

void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	(void)dev;
	(void)flags;
	assert(!allocation);
	allocation = calloc(1, size);
	return allocation;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data)
{
	(void)dev;
	assert(data == allocation);
	release_action = action;
	return 0;
}

struct clk *devm_clk_get(struct device *dev, const char *name)
{
	(void)dev;
	assert(!strcmp(name, "xtal"));
	return &xtal;
}

int clk_prepare_enable(struct clk *clock)
{
	clock->users++;
	return 0;
}

void clk_disable_unprepare(struct clk *clock)
{
	assert(clock->users);
	clock->users--;
}

struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name)
{
	(void)node;
	assert(!strcmp(name, "sprd,pmic"));
	return &registers;
}

bool device_property_read_bool(struct device *dev, const char *name)
{
	assert(!strcmp(name, "fplinux,headphones-only"));
	return dev->headphones_only;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
	assert(false);
}

void udelay(unsigned int microseconds)
{
	(void)microseconds;
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
	assert(minimum <= maximum);
}

void msleep(unsigned int milliseconds)
{
	(void)milliseconds;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	assert(reg % 4 == 0 && reg / 4 < 1024);
	*value = map->words[reg / 4];
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	assert(reg % 4 == 0 && reg / 4 < 1024);
	map->words[reg / 4] = (map->words[reg / 4] & ~mask) | (value & mask);
	map->writes++;
	return 0;
}

int regmap_write_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		      unsigned int value)
{
	return regmap_update_bits(map, reg, mask, value);
}

int ums9117_sc2720_calibrate_headphones(struct device *dev, struct regmap *map)
{
	(void)dev;
	assert(map == &registers);
	return 0;
}

static struct ums9117_sc2720_codec *create_codec(struct device *dev)
{
	struct ums9117_sc2720_codec *codec;

	memset(&registers, 0, sizeof(registers));
	registers.words[0x0c00 / 4] = 0xa003;
	registers.words[0x0c04 / 4] = 0x2720;
	registers.words[0x072c / 4] = 0x1356;
	codec = ums9117_sc2720_codec_create(dev);
	assert(!IS_ERR(codec));
	return codec;
}

static void release_codec(void)
{
	assert(release_action);
	release_action(allocation);
	assert(!xtal.users);
	free(allocation);
	allocation = NULL;
	release_action = NULL;
}

static void test_headphones_only_policy_rejects_speaker_routes(void)
{
	struct device dev = { .headphones_only = true };
	struct ums9117_sc2720_codec *codec = create_codec(&dev);
	unsigned int writes = registers.writes;
	unsigned int outputs;

	assert(!ums9117_sc2720_codec_has_speaker(codec));
	for (outputs = 2; outputs <= 3; outputs++) {
		assert(ums9117_sc2720_codec_prepare(codec, outputs, 0x0a) ==
		       -EOPNOTSUPP);
		assert(ums9117_sc2720_codec_set_outputs(codec, outputs, 0x0a) ==
		       -EOPNOTSUPP);
	}
	assert(registers.writes == writes);
#if CONFIG_SND_UMS9117_SPEAKER_VIBRATOR
	assert(ums9117_sc2720_codec_set_vibration(codec, true) == -EOPNOTSUPP);
	assert(registers.writes == writes);
#endif
	assert(!ums9117_sc2720_codec_prepare(codec, 1, 0x0a));
	assert(!ums9117_sc2720_codec_enable(codec));
	assert((registers.words[0x072c / 4] & 0xf000) == 0x1000);
	assert(!ums9117_sc2720_codec_disable(codec));
	assert(registers.words[0x072c / 4] == 0x1356);
	release_codec();
}

int main(void)
{
	test_headphones_only_policy_rejects_speaker_routes();
	puts("codec headphone-only board policy: ok");
	return 0;
}
