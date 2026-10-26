// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/property.h>

#include "qin-audio-profile.h"

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

static u32 registers[256];
static unsigned int ignored_write;
static int firmware_error;
static int property_error;
static unsigned int requests;
static unsigned int releases;
static struct firmware supplied;

u32 readl(const volatile void *address)
{
	return *(const volatile u32 *)address;
}

void writel(u32 value, volatile void *address)
{
	size_t offset =
		(const volatile u8 *)address - (const volatile u8 *)registers;

	CHECK(offset < sizeof(registers) && !(offset & 3));
	if (offset == ignored_write)
		return;
	*(volatile u32 *)address = value;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

void dev_info(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
}

int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	(void)dev;
	CHECK(!strcmp(name, "firmware-name"));
	*value = "qin1splus-audio-profile.bin";
	return property_error;
}

int request_firmware(const struct firmware **firmware, const char *name,
		     struct device *dev)
{
	(void)dev;
	CHECK(!strcmp(name, "qin1splus-audio-profile.bin"));
	requests++;
	*firmware = &supplied;
	return firmware_error;
}

void release_firmware(const struct firmware *firmware)
{
	CHECK(firmware == &supplied);
	releases++;
}

static void write_s16(u8 *destination, s16 value)
{
	destination[0] = (u16)value;
	destination[1] = (u16)value >> 8;
}

static void make_profile(u8 bytes[115])
{
	static const u8 header[21] = {
		'F', 'P', 'L', 'Q', 'I', 'N', 'A', 'U', 0x44, 0xac, 0,
		0,   0,	  25,  1,   6,	 127, 1,   1,	0xfc, 7,
	};
	static const s16 first[6] = { 2048, 100, -200, -300, 400, -500 };
	unsigned int section;
	unsigned int i;

	memset(bytes, 0, 115);
	memcpy(bytes, header, sizeof(header));
	for (i = 0; i < 11; i++)
		write_s16(bytes + 21 + i * 2, i == 4 ? -30 : (s16)(i + 1));
	for (section = 0; section < 6; section++) {
		write_s16(bytes + 43 + section * 12, 4096);
		write_s16(bytes + 45 + section * 12, 16384);
	}
	for (i = 0; i < 6; i++)
		write_s16(bytes + 43 + i * 2, first[i]);
}

static void check_parser_and_firmware_lifetime(void)
{
	struct qin_audio_profile profile;
	struct device device = {};
	u8 bytes[115];

	make_profile(bytes);
	CHECK(qin_audio_profile_parse(bytes, sizeof(bytes), &profile) == 0);
	CHECK(profile.processing_rate == 44100);
	CHECK(profile.dac_gain == 0 && profile.digital_gain == 25);
	CHECK(profile.mainmic_gain == 6 && profile.limit == 127);
	CHECK(profile.eq_enabled && profile.alc_enabled &&
	      profile.digital_gain_enabled);
	CHECK(profile.output_scale == 2044 && profile.alc[4] == -30);
	CHECK(profile.section[0].b1 == -200 &&
	      profile.section[0].minus_a2 == -500);
	CHECK(qin_audio_profile_parse(bytes, 114, &profile) == -EINVAL);
	bytes[8]++;
	CHECK(qin_audio_profile_parse(bytes, sizeof(bytes), &profile) ==
	      -EINVAL);
	make_profile(bytes);
	write_s16(bytes + 53, 16384);
	CHECK(qin_audio_profile_parse(bytes, sizeof(bytes), &profile) ==
	      -EINVAL);
	make_profile(bytes);
	supplied = (struct firmware){ .data = bytes, .size = sizeof(bytes) };
	firmware_error = -ENOENT;
	CHECK(qin_audio_profile_load(&device, &profile) == -ENODEV);
	CHECK(requests == 1 && releases == 0);
	firmware_error = 0;
	bytes[0] = 'X';
	CHECK(qin_audio_profile_load(&device, &profile) == -EINVAL);
	CHECK(requests == 2 && releases == 1);
	make_profile(bytes);
	CHECK(qin_audio_profile_load(&device, &profile) == 0);
	CHECK(requests == 3 && releases == 2);
	property_error = -EINVAL;
	CHECK(qin_audio_profile_load(&device, &profile) == -EINVAL);
	CHECK(requests == 3 && releases == 2);
}

static void check_processing_restore_and_failed_writes(void)
{
	struct qin_audio_profile profile;
	struct qin_audio_processing_state state = {};
	struct device device = {};
	u32 previous[256];
	u8 bytes[115];
	unsigned int i;

	make_profile(bytes);
	CHECK(qin_audio_profile_parse(bytes, sizeof(bytes), &profile) == 0);
	for (i = 0; i < 256; i++)
		registers[i] = 0x12340000 + i;
	registers[0x48 / 4] = 0x22;
	memcpy(previous, registers, sizeof(previous));
	ignored_write = ~0U;
	CHECK(qin_audio_processing_prepare(&device, registers, &profile,
					   &state) == 0);
	CHECK(state.active);
	CHECK(registers[0x48 / 4] == 0xa7f);
	CHECK(registers[0x100 / 4] == 2048 && registers[0x108 / 4] == 100);
	CHECK(registers[0x110 / 4] == 16384 && registers[0x118 / 4] == 0xff38);
	CHECK(registers[0x120 / 4] == 0xfed4 && registers[0x128 / 4] == 400);
	CHECK(registers[0x130 / 4] == 0xfe0c && registers[0x250 / 4] == 2044);
	CHECK(registers[0x10c / 4] == 0 && registers[0x254 / 4] == 0);
	CHECK(registers[0x5c / 4] == 0xffffffe2);
	CHECK(registers[0x40 / 4] == previous[0x40 / 4]);
	CHECK(registers[0x44 / 4] == previous[0x44 / 4]);
	CHECK(qin_audio_processing_prepare(&device, registers, &profile,
					   &state) == -EBUSY);
	ignored_write = 0x5c;
	CHECK(qin_audio_processing_restore(&device, registers, &state) == -EIO);
	CHECK(state.active && !(registers[0x48 / 4] & 0xa00));
	ignored_write = ~0U;
	CHECK(qin_audio_processing_restore(&device, registers, &state) == 0);
	CHECK(!state.active);
	for (i = 0; i < 86; i++)
		CHECK(registers[0x100 / 4 + i] == previous[0x100 / 4 + i]);
	for (i = 0; i < 11; i++)
		CHECK(registers[0x4c / 4 + i] == previous[0x4c / 4 + i]);
	ignored_write = 0x118;
	CHECK(qin_audio_processing_prepare(&device, registers, &profile,
					   &state) == -EIO);
	CHECK(state.active);
	ignored_write = ~0U;
	CHECK(qin_audio_processing_restore(&device, registers, &state) == 0);
	CHECK(!state.active);
}

int main(void)
{
	check_parser_and_firmware_lifetime();
	check_processing_restore_and_failed_writes();
	return 0;
}
