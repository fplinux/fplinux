// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host component: linked SPI transport, fake MMIO, resources and time.
 * Captured TX bytes and controller command/data, channel and CS selection
 * check the handoff contract. The fake does not serialize an electrical bus
 * or execute LCDC DMA, so this is not a panel or physical timing test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/ktime.h>

#include "ums9117-drm-internal.h"

struct transmitted_word {
	u32 value;
	u32 mode;
	unsigned int bits;
	bool data;
	bool selected;
};

static struct {
	u32 spi[26];
	u32 lcm[6];
	u32 clock_selector;
	u32 reset_set;
	u32 reset_clear;
	struct transmitted_word words[8];
	u32 armed_words[8];
	size_t word_count;
	size_t armed_count;
	unsigned int selects;
	unsigned int deselects;
	ktime_t now;
	ktime_t completion_at;
	bool stalled;
} peripheral;
static unsigned int failures;
static struct resource spi_resource = { .start = 0x21000000 };

static const struct ums9117_drm_profile raw16_profile = {
	.spi_native_16bit = true,
	.spi_init_clock_divider = 1,
	.spi_frame_clock_divider = 1,
};
static const struct ums9117_drm_profile default_profile = {
	.spi_frame_clock_divider = 3,
};

#define CHECK(condition, message)                                       \
	do {                                                            \
		if (!(condition)) {                                     \
			fprintf(stderr, "%s: %s\n", __func__, message); \
			failures++;                                     \
		}                                                       \
	} while (0)

static void complete_transfer(void)
{
	if (peripheral.completion_at && !peripheral.stalled &&
	    peripheral.now >= peripheral.completion_at) {
		peripheral.spi[0x034 / 4] = 0x80;
		peripheral.spi[0x028 / 4] = 0x100;
		peripheral.completion_at = 0;
	}
}

ktime_t ktime_get(void)
{
	return peripheral.now;
}

void udelay(unsigned long delay)
{
	peripheral.now += delay;
	complete_transfer();
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
	(void)maximum;
	udelay(minimum);
}

u32 readl(const void *address)
{
	complete_transfer();
	return *(const u32 *)address;
}

void writel(u32 value, void *address)
{
	if (address == &peripheral.spi[0x008 / 4]) {
		bool was_selected = !(peripheral.spi[0x008 / 4] & 0x100);
		bool selected = !(value & 0x100);

		if (!was_selected && selected)
			peripheral.selects++;
		if (was_selected && !selected)
			peripheral.deselects++;
	}
	*(u32 *)address = value;
	if (address == &peripheral.spi[0x024 / 4])
		peripheral.spi[0x028 / 4] &= ~value;
	if (address == &peripheral.spi[0x064 / 4] && (value & 2)) {
		if (peripheral.armed_count ==
		    ARRAY_SIZE(peripheral.armed_words))
			abort();
		peripheral.armed_words[peripheral.armed_count++] =
			((peripheral.spi[0x054 / 4] & 0x3ff) << 16) |
			(peripheral.spi[0x058 / 4] & 0xffff);
	}
	if (address == &peripheral.spi[0]) {
		struct transmitted_word *word;

		if (peripheral.word_count == ARRAY_SIZE(peripheral.words))
			abort();
		word = &peripheral.words[peripheral.word_count++];
		*word = (struct transmitted_word){
			.value = value,
			.mode = peripheral.spi[0x04c / 4] & 0x18,
			.bits = (peripheral.spi[0x008 / 4] >> 2) & 0x1f,
			.data = !!(peripheral.spi[0x054 / 4] & 0x8000),
			.selected = !(peripheral.spi[0x008 / 4] & 0x100),
		};
		peripheral.spi[0x034 / 4] = 0x100;
		peripheral.spi[0x028 / 4] = 0;
		peripheral.completion_at = peripheral.now + 7;
	}
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	(void)map;
	(void)reg;
	(void)value;
	return 0;
}

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	(void)pdev;
	return type == IORESOURCE_MEM && !strcmp(name, "spi") ? &spi_resource :
								NULL;
}

void *devm_ioremap_resource(struct device *dev, struct resource *resource)
{
	(void)dev;
	return resource == &spi_resource ? peripheral.spi :
					   (void *)(intptr_t)-EINVAL;
}

void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name)
{
	(void)pdev;
	if (!strcmp(name, "lcm"))
		return peripheral.lcm;
	if (!strcmp(name, "spi-clock-selector"))
		return &peripheral.clock_selector;
	if (!strcmp(name, "spi-reset-set"))
		return &peripheral.reset_set;
	if (!strcmp(name, "spi-reset-clear"))
		return &peripheral.reset_clear;
	return (void *)(intptr_t)-EINVAL;
}

static struct ums9117_drm initialize(const struct ums9117_drm_profile *profile)
{
	struct platform_device pdev = {};
	struct ums9117_drm udrm = { .profile = profile };
	int result;

	memset(&peripheral, 0, sizeof(peripheral));
	peripheral.spi[0x034 / 4] = 0x80;
	result = ums9117_drm_spi_init_transport(&udrm, &pdev);
	CHECK(result == 0, "named resources must initialize SPI transport");
	CHECK(udrm.stream_phys == 0x21000000,
	      "pixel consumer must target the SPI TX port");
	result = ums9117_drm_spi_enable_transport(&udrm);
	CHECK(result == 0, "SPI transport must enable");
	CHECK(peripheral.spi[0x008 / 4] & 0x100,
	      "enable must leave the panel deselected");
	peripheral.selects = 0;
	peripheral.deselects = 0;
	return udrm;
}

static void check_word(size_t index, u32 value, bool data, u32 mode)
{
	const struct transmitted_word *word = &peripheral.words[index];

	CHECK(word->value == value, "DCS byte must reach TX unchanged");
	CHECK(word->data == data, "command/data selection must match the byte");
	CHECK(word->bits == 8, "DCS bytes must use the 8-bit channel");
	CHECK(word->mode == mode, "DCS must preserve the profile wire mode");
	CHECK(word->selected,
	      "DCS bytes must be transmitted with CS0 selected");
}

static void raw16_dcs(void)
{
	struct ums9117_drm udrm = initialize(&raw16_profile);
	const u8 data[] = { 0x55, 0xaa };
	int result;

	CHECK(peripheral.spi[0x004 / 4] == 1,
	      "raw16 initialization must use the profile clock divider");
	CHECK(peripheral.spi[0x04c / 4] == 0x18,
	      "raw16 initialization must select raw mode 3");
	result = ums9117_drm_spi_post_reset(&udrm);
	CHECK(result == 0, "post-reset must succeed");
	result = ums9117_drm_spi_dcs(&udrm, 0x3a, data, sizeof(data));
	CHECK(result == 0, "completed command and parameters must succeed");
	CHECK(peripheral.word_count == 3,
	      "DCS must send one command and two bytes");
	check_word(0, 0x3a, false, 0x18);
	check_word(1, 0x55, true, 0x18);
	check_word(2, 0xaa, true, 0x18);
	CHECK(peripheral.selects == 3 && peripheral.deselects == 3,
	      "raw16 must delimit each DCS byte with a CS0 transaction");
	CHECK(peripheral.spi[0x008 / 4] & 0x100,
	      "completed ordinary DCS must release CS0");
	CHECK(((peripheral.spi[0x008 / 4] >> 2) & 0x1f) == 16,
	      "pixel consumer must receive a 16-bit channel");
	CHECK(peripheral.spi[0x04c / 4] == 0x18,
	      "pixel consumer must retain raw mode without RGB565 expansion");
	CHECK(peripheral.spi[0x054 / 4] & 0x8000,
	      "pixel consumer must receive data selection");
}

static void default_dcs(void)
{
	struct ums9117_drm udrm = initialize(&default_profile);
	const u8 data = 0x55;
	int result;

	CHECK(peripheral.spi[0x004 / 4] == 3,
	      "omitted initialization divider must preserve the default clock");
	CHECK(peripheral.spi[0x04c / 4] == 8,
	      "default profile must select packed three-wire mode");
	result = ums9117_drm_spi_post_reset(&udrm);
	CHECK(result == 0, "post-reset must succeed");
	CHECK(!(peripheral.spi[0x008 / 4] & 0x100),
	      "default post-reset must select CS0 for the init sequence");
	result = ums9117_drm_spi_dcs(&udrm, 0x3a, &data, 1);
	CHECK(result == 0, "default command and parameter must succeed");
	CHECK(peripheral.word_count == 2,
	      "DCS must send command and parameter");
	check_word(0, 0x3a, false, 8);
	check_word(1, 0x55, true, 8);
	CHECK(peripheral.selects == 1 && peripheral.deselects == 0,
	      "default DCS must preserve shared CS0 ownership");
	CHECK(((peripheral.spi[0x008 / 4] >> 2) & 0x1f) == 17,
	      "default pixel consumer must retain the 17-bit channel");
	CHECK(peripheral.spi[0x04c / 4] == 0x4008,
	      "default pixel consumer must retain RGB565 expansion");
}

static void raw16_frame(void)
{
	struct ums9117_drm udrm = initialize(&raw16_profile);
	int result = ums9117_drm_spi_begin_frame(&udrm, 76800);

	CHECK(result == 0, "completed RAMWR must admit a 320 by 240 frame");
	CHECK(peripheral.word_count == 1, "frame handoff must issue RAMWR");
	check_word(0, 0x2c, false, 0x18);
	CHECK(peripheral.selects == 1 && peripheral.deselects == 0,
	      "successful RAMWR must retain CS0 for the pixel consumer");
	CHECK(!(peripheral.spi[0x008 / 4] & 0x100),
	      "pixel stream must start with CS0 selected");
	CHECK(peripheral.spi[0x004 / 4] == 1,
	      "frame must use its profile clock divider");
	CHECK(peripheral.spi[0x04c / 4] == 0x98,
	      "raw pixel stream must retain transmit hold and raw mode");
	CHECK(((peripheral.spi[0x008 / 4] >> 2) & 0x1f) == 16,
	      "raw pixel stream must use the 16-bit channel");
	CHECK(peripheral.spi[0x054 / 4] == 0x8001 &&
		      peripheral.spi[0x058 / 4] == 0x2c00,
	      "frame transfer length must encode exactly 76800 data words");
	CHECK(peripheral.armed_count == 2 && peripheral.armed_words[0] == 1 &&
		      peripheral.armed_words[1] == 76800,
	      "consumer handoff must arm one RAMWR byte then the full pixel count");
}

static void default_frame(void)
{
	struct ums9117_drm udrm = initialize(&default_profile);
	int result = ums9117_drm_spi_post_reset(&udrm);

	CHECK(result == 0, "default post-reset must succeed");
	result = ums9117_drm_spi_begin_frame(&udrm, 30720);
	CHECK(result == 0, "default frame must admit its pixel count");
	CHECK(peripheral.word_count == 1, "default frame must issue RAMWR");
	check_word(0, 0x2c, false, 8);
	CHECK(peripheral.selects == 1 && peripheral.deselects == 0,
	      "default frame must preserve selected CS0");
	CHECK(peripheral.spi[0x004 / 4] == 3,
	      "default frame must use its profile clock divider");
	CHECK(peripheral.spi[0x04c / 4] == 0x4088,
	      "default frame must retain RGB565 expansion and transmit hold");
	CHECK(((peripheral.spi[0x008 / 4] >> 2) & 0x1f) == 17,
	      "default frame must preserve the 17-bit pixel channel");
	CHECK(peripheral.spi[0x054 / 4] == 0x8000 &&
		      peripheral.spi[0x058 / 4] == 0x7800,
	      "default frame transfer length must encode exactly 30720 data words");
}

static void raw16_timeout(void)
{
	struct ums9117_drm udrm = initialize(&raw16_profile);
	ktime_t started = peripheral.now;
	int result;

	peripheral.stalled = true;
	result = ums9117_drm_spi_begin_frame(&udrm, 76800);
	CHECK(result == -ETIMEDOUT, "stalled RAMWR must reject the frame");
	CHECK(peripheral.word_count == 1,
	      "timeout must occur after RAMWR byte");
	check_word(0, 0x2c, false, 0x18);
	CHECK(peripheral.now - started == 250000,
	      "stalled DCS must terminate at its bounded virtual deadline");
	CHECK(peripheral.selects == 1 && peripheral.deselects == 1,
	      "failed RAMWR must release CS0");
	CHECK(peripheral.spi[0x008 / 4] & 0x100,
	      "failed frame must leave CS0 deselected");
	CHECK(peripheral.armed_count == 1 && peripheral.armed_words[0] == 1,
	      "failed RAMWR must never arm a pixel transfer");
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "raw16-dcs"))
		raw16_dcs();
	else if (!strcmp(argv[1], "default-dcs"))
		default_dcs();
	else if (!strcmp(argv[1], "raw16-frame"))
		raw16_frame();
	else if (!strcmp(argv[1], "default-frame"))
		default_frame();
	else if (!strcmp(argv[1], "raw16-timeout"))
		raw16_timeout();
	else
		return 2;
	return failures ? 1 : 0;
}
