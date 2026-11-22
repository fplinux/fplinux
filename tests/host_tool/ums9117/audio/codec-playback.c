// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "host.h"
#include "ums9117-sc2720-codec.h"

/* Register storage and no-op time replace the host's absent analog hardware. */
static struct regmap registers;
static struct clk xtal;
static void (*release_action)(void *);
static void *allocation;
static bool preempt_disabled;
static bool irqs_disabled;
static unsigned int pulse_delays[7];
static size_t delay_count;
static unsigned int off_waits;

struct gpio_desc {
	int value;
	unsigned int requests;
	unsigned int fail_at;
	unsigned int attempts;
	int levels[7];
	size_t level_count;
};

static struct gpio_desc speaker;

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
	if (!strcmp(name, "fplinux,headphone-fed-speaker"))
		return dev->headphone_fed_speaker;
	assert(!strcmp(name, "fplinux,headphones-only"));
	return dev->headphones_only;
}

int device_property_count_u32(struct device *dev, const char *name)
{
	assert(!strcmp(name, "fplinux,speaker-enable-delays-us"));
	return dev->speaker_enable_edges;
}

int device_property_read_u32_array(struct device *dev, const char *name,
				   u32 *values, size_t count)
{
	assert(!strcmp(name, "fplinux,speaker-enable-delays-us"));
	assert(count == (size_t)dev->speaker_enable_edges);
	memcpy(values, dev->speaker_enable_delays_us, count * sizeof(*values));
	return 0;
}

int device_property_read_u32(struct device *dev, const char *name, u32 *value)
{
	assert(!strcmp(name, "fplinux,speaker-off-delay-us"));
	if (!dev->speaker_off_delay_us)
		return -EINVAL;
	*value = dev->speaker_off_delay_us;
	return 0;
}

struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name,
				 enum gpiod_flags flags)
{
	assert(dev->headphone_fed_speaker);
	assert(!strcmp(name, "speaker-enable"));
	assert(flags == GPIOD_ASIS);
	speaker.requests++;
	return &speaker;
}

bool gpiod_cansleep(struct gpio_desc *gpio)
{
	assert(gpio == &speaker);
	return false;
}

int gpiod_direction_output(struct gpio_desc *gpio, int value)
{
	return gpiod_set_value_cansleep(gpio, value);
}

int gpiod_set_value(struct gpio_desc *gpio, int value)
{
	assert(gpio == &speaker);
	assert(irqs_disabled && preempt_disabled);
	assert((registers.words[0x0728 / 4] & 0x0340) == 0x0300);
	if (++gpio->attempts == gpio->fail_at)
		return -EIO;
	assert(gpio->level_count < ARRAY_SIZE(gpio->levels));
	gpio->levels[gpio->level_count++] = value;
	gpio->value = value;
	return 0;
}

int gpiod_set_value_cansleep(struct gpio_desc *gpio, int value)
{
	assert(gpio == &speaker && value == 0);
	assert(!irqs_disabled && !preempt_disabled);
	gpio->value = value;
	return 0;
}

void preempt_disable(void)
{
	assert(!preempt_disabled);
	preempt_disabled = true;
}

void preempt_enable(void)
{
	assert(preempt_disabled && !irqs_disabled);
	preempt_disabled = false;
}

unsigned long codec_playback_irq_save(void)
{
	assert(preempt_disabled && !irqs_disabled);
	irqs_disabled = true;
	return 0;
}

void codec_playback_irq_restore(unsigned long flags)
{
	assert(flags == 0 && irqs_disabled);
	irqs_disabled = false;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
	assert(false);
}

void udelay(unsigned int microseconds)
{
	if (irqs_disabled) {
		assert(preempt_disabled &&
		       delay_count < ARRAY_SIZE(pulse_delays));
		pulse_delays[delay_count++] = microseconds;
	}
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
	assert(minimum <= maximum);
	assert(!irqs_disabled && !preempt_disabled);
	if (minimum == 1000 && maximum == 1100)
		off_waits++;
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

static void test_amplifier_stays_low_during_idle_capture_and_failed_enable(void)
{
	const u32 mode4_delays[] = { 2, 2, 2, 2, 2, 2, 2 };
	const int mode4_levels[] = { 1, 0, 1, 0, 1, 0, 1 };
	struct device dev = {
		.headphone_fed_speaker = true,
		.speaker_enable_delays_us = mode4_delays,
		.speaker_enable_edges = ARRAY_SIZE(mode4_delays),
		.speaker_off_delay_us = 1000,
	};
	struct ums9117_sc2720_codec *codec = create_codec(&dev);

	assert(ums9117_sc2720_codec_has_speaker(codec));
	assert(speaker.requests == 1 && speaker.value == 0 && off_waits == 1);
	assert(!ums9117_sc2720_codec_prepare(
		codec, UMS9117_SC2720_OUTPUT_SPEAKER, 0x0a));
	assert(ums9117_sc2720_codec_set_speaker_mute(codec, false) >= 0);
	assert(!ums9117_sc2720_codec_enable(codec));
	assert(!ums9117_sc2720_codec_prepare_capture(
		codec, UMS9117_SC2720_CAPTURE_INTERNAL));
	assert(!ums9117_sc2720_codec_enable_capture(codec));
	assert(speaker.value == 0 && speaker.attempts == 0);
	assert(!ums9117_sc2720_codec_disable_capture(codec));

	speaker.fail_at = 2;
	assert(ums9117_sc2720_codec_set_playback_active(codec, true) == -EIO);
	assert(speaker.value == 0 && off_waits == 2);
	assert(speaker.level_count == 2 && speaker.levels[0] == 1 &&
	       speaker.levels[1] == 0);
	assert(!irqs_disabled && !preempt_disabled);

	speaker.level_count = 0;
	delay_count = 0;
	assert(!ums9117_sc2720_codec_set_playback_active(codec, true));
	assert(speaker.value == 1 && speaker.level_count == 7);
	assert(!memcmp(speaker.levels, mode4_levels, sizeof(mode4_levels)));
	assert(delay_count == 7 &&
	       !memcmp(pulse_delays, mode4_delays, sizeof(mode4_delays)));
	assert(!ums9117_sc2720_codec_set_playback_active(codec, false));
	assert(speaker.value == 0 && off_waits == 3);
	assert(!ums9117_sc2720_codec_disable(codec));
	release_codec();
}

static void test_incomplete_amplifier_configuration_rejects_gpio_access(void)
{
	const u32 unfinished_pulse[] = { 2, 2 };
	struct device dev = {
		.headphone_fed_speaker = true,
		.speaker_enable_delays_us = unfinished_pulse,
		.speaker_enable_edges = ARRAY_SIZE(unfinished_pulse),
		.speaker_off_delay_us = 1000,
	};

	memset(&speaker, 0, sizeof(speaker));
	assert(PTR_ERR(ums9117_sc2720_codec_create(&dev)) == -EINVAL);
	assert(speaker.requests == 0);
	free(allocation);
	allocation = NULL;
}

int main(void)
{
	test_headphones_only_policy_rejects_speaker_routes();
	test_amplifier_stays_low_during_idle_capture_and_failed_enable();
	test_incomplete_amplifier_configuration_rejects_gpio_access();
	puts("codec playback routes: ok");
	return 0;
}
