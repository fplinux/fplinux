// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>

#include "host.h"
#include "ums9117-audio.h"

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

/* Kernel resource and MMIO boundaries; no DSP audio or readiness algorithm. */
static u32 aud[1024], vbc[2048], clocks[6], matrix;
static struct regmap aon;
static struct pinctrl control;
static struct pinctrl_state pins;
static void *allocation;
static unsigned int clocked_alc_clears;

u32 readl(const volatile void *address)
{
	return *(const volatile u32 *)address;
}

void writel(u32 value, volatile void *address)
{
	if (address == vbc && (value & 0x800) && (aud[0] & 5) == 5)
		clocked_alc_clears++;
	*(volatile u32 *)address = value;
}

void udelay(unsigned int delay)
{
	(void)delay;
}
void usleep_range(unsigned int minimum, unsigned int maximum)
{
	(void)minimum;
	(void)maximum;
}

void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	(void)dev;
	(void)flags;
	CHECK(!allocation);
	allocation = calloc(1, size);
	return allocation;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name)
{
	(void)pdev;
	if (!strcmp(name, "aud"))
		return aud;
	if (!strcmp(name, "vbc"))
		return vbc;
	if (!strcmp(name, "clk1"))
		return clocks;
	CHECK(!strcmp(name, "iis-matrix"));
	return &matrix;
}

bool device_property_read_bool(struct device *dev, const char *name)
{
	(void)dev;
	if (!strcmp(name, "fplinux,external-fm"))
		return false;
	CHECK(!strcmp(name, "fplinux,microphone-capture"));
	return false;
}

struct pinctrl *devm_pinctrl_get(struct device *dev)
{
	(void)dev;
	return &control;
}

struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *selected,
					   const char *name)
{
	CHECK(selected == &control && !strcmp(name, "audio"));
	return &pins;
}

int pinctrl_select_state(struct pinctrl *selected, struct pinctrl_state *state)
{
	CHECK(selected == &control && state == &pins);
	return 0;
}

struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name)
{
	(void)node;
	CHECK(!strcmp(name, "fplinux,aon-apb"));
	return &aon;
}

int regmap_read(struct regmap *map, unsigned int offset, unsigned int *value)
{
	CHECK(map == &aon && offset < sizeof(aon.words));
	*value = map->words[offset / 4];
	return 0;
}

int regmap_write(struct regmap *map, unsigned int offset, unsigned int value)
{
	CHECK(map == &aon);
	if (offset >= 0x2000)
		map->words[(offset - 0x2000) / 4] &= ~value;
	else if (offset >= 0x1000)
		map->words[(offset - 0x1000) / 4] |= value;
	else
		map->words[offset / 4] = value;
	return 0;
}

static void test_start_protocol(bool alc_enabled)
{
	struct ums9117_audio_dac_processing processing = {
		.output_scale = 1234,
		.alc_enabled = true,
		.alc = { 3, 4, 5, 6, -7, 8, 9, 10, 11, 12, 13 },
	};
	struct platform_device device = {};
	struct ums9117_audio *audio;
	unsigned int clears;

	memset(aud, 0, sizeof(aud));
	memset(vbc, 0, sizeof(vbc));
	memset(clocks, 0, sizeof(clocks));
	memset(&aon, 0, sizeof(aon));
	matrix = 0;
	clocked_alc_clears = 0;
	/* Empty FIFO status and completed mute status are external responses. */
	vbc[0xbc / 4] = vbc[0xc0 / 4] = 0x40000;
	aud[0x20 / 4] = 2;
	for (unsigned int rate = 0; rate < 3; rate++)
		for (unsigned int section = 0; section < 6; section++)
			processing.section[rate][section] =
				(struct ums9117_audio_eq_section){
					.scale = 4096, .b0 = 16384
				};
	audio = ums9117_audio_create(&device);
	CHECK(!IS_ERR(audio));
	CHECK(ums9117_audio_set_dac_processing(audio, &processing, true,
					       alc_enabled) == 0);
	CHECK(ums9117_audio_prepare(audio, 48000) == 0);
	ums9117_audio_write(audio, 0, 0);
	ums9117_audio_start(audio);
	/* Independent hardware literals: TOP DAC bits0/2 and ALC enable bit11. */
	CHECK((aud[0] & 5) == 5);
	CHECK(!!(vbc[0x48 / 4] & 0x800) == alc_enabled);
	if (alc_enabled)
		CHECK(clocked_alc_clears >= 1);
	CHECK(vbc[0x250 / 4] == 0x04d2);
	CHECK(vbc[0x5c / 4] == 0xfff9);
	CHECK(vbc[0x108 / 4] == 0x4000);
	CHECK(vbc[0x110 / 4] == 0x4000);
	clears = clocked_alc_clears;
	ums9117_audio_start(audio);
	CHECK(clocked_alc_clears == clears);
	ums9117_audio_stop(audio);
	ums9117_audio_release(audio);
	free(allocation);
	allocation = NULL;
}

int main(void)
{
	test_start_protocol(true);
	test_start_protocol(false);
	puts("ALC host register start protocol: ok");
	return 0;
}
