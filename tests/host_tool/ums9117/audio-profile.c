// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/property.h>

#include "ums9117-audio-profile.h"

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

/* Literal packed-format examples; offsets are independent of native structs. */
static const u8 headphone_gains[9] = { 127, 120, 100, 80, 60, 40, 20, 10, 0 };
static const u8 speaker_gains[9] = { 90, 80, 70, 60, 50, 40, 30, 20, 10 };
static const u8 combined_gains[9] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
static const s16 alc_words[11] = { -32768, -2, 0, 1, 32767, 5, 6, 7, 8, 9, 10 };

static void make_profile_bytes(u8 bytes[822])
{
	static const u8 processing_header[25] = {
		0x00, 0x10, 0x00, 0x00, 0x80, 0xfe, 0xff, 0x00, 0x00,
		0x01, 0x00, 0xff, 0x7f, 0x05, 0x00, 0x06, 0x00, 0x07,
		0x00, 0x08, 0x00, 0x09, 0x00, 0x0a, 0x00,
	};
	static const u8 unity_section[12] = {
		0x00, 0x10, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	static const u8 tone[34] = {
		0,    0,    0,	  0,	0,    0,    0,	  0,	0,
		0,    0,    0,	  0,	0,    0,    0x40, 0,	0,
		0,    0x40, 0,	  0,	0,    0x40, 0x34, 0x12, 0x78,
		0x56, 0xbc, 0x9a, 0xf0, 0xde, 0x23, 0x01,
	};
	static const size_t routes[3] = { 65, 306, 547 };
	size_t route;
	size_t section;

	memset(bytes, 0, 822);
	memcpy(bytes, "FPAUDIO", 8);
	memcpy(bytes + 8, "fplinux,test-phone", 18);
	bytes[32] = 2;
	memcpy(bytes + 33, headphone_gains, 9);
	bytes[42] = 255;
	memcpy(bytes + 44, speaker_gains, 9);
	bytes[53] = 17;
	bytes[55] = 7;
	memcpy(bytes + 56, combined_gains, 9);
	for (route = 0; route < 3; route++) {
		memcpy(bytes + routes[route], processing_header, 25);
		bytes[routes[route] + 2] = route != 0;
		for (section = 0; section < 18; section++)
			memcpy(bytes + routes[route] + 25 + section * 12,
			       unity_section, 12);
	}
	memcpy(bytes + 788, tone, 34);
}

static void
check_processing(const struct ums9117_audio_dac_processing *processing,
		 bool alc_enabled)
{
	size_t rate;
	size_t section;
	size_t word;

	CHECK(processing->output_scale == 4096);
	CHECK(processing->alc_enabled == alc_enabled);
	for (word = 0; word < 11; word++)
		CHECK(processing->alc[word] == alc_words[word]);
	for (rate = 0; rate < 3; rate++) {
		for (section = 0; section < 6; section++) {
			const struct ums9117_audio_eq_section *eq =
				&processing->section[rate][section];

			CHECK(eq->scale == 4096);
			CHECK(eq->b0 == 16384);
			CHECK(eq->b1 == 0);
			CHECK(eq->minus_a1 == 0);
			CHECK(eq->b2 == 0);
			CHECK(eq->minus_a2 == 0);
		}
	}
}

static void check_decoded_profile(const struct ums9117_audio_profile *profile)
{
	CHECK(profile->fitted);
	CHECK(profile->codec_volume == 1);
	CHECK(profile->combined_codec_volume == 6);
	CHECK(profile->speaker_pa_word == 255);
	CHECK(profile->combined_pa_word == 17);
	CHECK(!memcmp(profile->dac_gain, headphone_gains, 9));
	CHECK(!memcmp(profile->speaker_dac_gain, speaker_gains, 9));
	CHECK(!memcmp(profile->combined_dac_gain, combined_gains, 9));
	check_processing(&profile->headphone_processing, false);
	check_processing(&profile->speaker_processing, true);
	check_processing(&profile->combined_processing, true);
}

static void check_decoded_tone(const struct ums9117_audio_vibrate_tone *tone)
{
	size_t rate;

	for (rate = 0; rate < 3; rate++) {
		CHECK(tone->sin[rate] == 0);
		CHECK(tone->cos[rate] == 1073741824);
	}
	CHECK(tone->level[0] == 0x1234);
	CHECK(tone->level[1] == 0x5678);
	CHECK(tone->fall == 0x9abc);
	CHECK(tone->rise == 0xdef0);
	CHECK(tone->hold == 0x0123);
}

static int parse(const u8 *bytes, size_t size, bool vibration,
		 struct ums9117_audio_profile *profile)
{
	return ums9117_audio_profile_parse("fplinux,test-phone", bytes, size,
					   vibration, profile);
}

static void test_decoded_values(void)
{
	struct ums9117_audio_profile profile = {};
	struct ums9117_audio_vibrate_tone untouched = {};
	u8 bytes[822];
	size_t group;
	size_t level;

	make_profile_bytes(bytes);
	CHECK(parse(bytes, 788, false, &profile) == 0);
	check_decoded_profile(&profile);
	CHECK(!memcmp(&profile.vibrate_tone, &untouched, sizeof(untouched)));
	memset(&profile, 0, sizeof(profile));
	CHECK(parse(bytes, 822, true, &profile) == 0);
	check_decoded_profile(&profile);
	if (CONFIG_SND_UMS9117_SPEAKER_VIBRATOR)
		check_decoded_tone(&profile.vibrate_tone);
	else
		CHECK(!memcmp(&profile.vibrate_tone, &untouched,
			      sizeof(untouched)));

	/* Both legal PGA extremes translate to codec levels, including combined. */
	bytes[32] = 7;
	bytes[55] = 2;
	bytes[42] = 0;
	bytes[53] = 255;
	for (group = 0; group < 2; group++) {
		memset(bytes + 33, group ? 127 : 0, 9);
		memset(bytes + 44, group ? 127 : 0, 9);
		memset(bytes + 56, group ? 127 : 0, 9);
		CHECK(parse(bytes, 788, false, &profile) == 0);
		CHECK(profile.codec_volume == 6);
		CHECK(profile.combined_codec_volume == 1);
		CHECK(profile.speaker_pa_word == 0);
		CHECK(profile.combined_pa_word == 255);
		for (level = 0; level < 9; level++) {
			CHECK(profile.dac_gain[level] == (group ? 127 : 0));
			CHECK(profile.speaker_dac_gain[level] ==
			      (group ? 127 : 0));
			CHECK(profile.combined_dac_gain[level] ==
			      (group ? 127 : 0));
		}
	}
}

struct invalid_field {
	size_t offset;
	u8 low;
	u8 high;
	bool word;
};

static void check_invalid_fields(const struct invalid_field *fields,
				 size_t count)
{
	struct ums9117_audio_profile profile;
	u8 bytes[822];
	size_t i;

	for (i = 0; i < count; i++) {
		make_profile_bytes(bytes);
		bytes[fields[i].offset] = fields[i].low;
		if (fields[i].word)
			bytes[fields[i].offset + 1] = fields[i].high;
		memset(&profile, 0, sizeof(profile));
		CHECK(parse(bytes, 788, false, &profile) == -22);
		CHECK(!profile.fitted);
	}
}

static void test_identity_and_length(void)
{
	static const struct invalid_field identity[] = {
		{ 0, 'X', 0, false }, { 7, '1', 0, false },
		{ 8, 'X', 0, false }, { 26, 1, 0, false },
		{ 31, 1, 0, false },
	};
	static const size_t no_tone_sizes[] = { 0, 42, 787, 789, 822 };
	static const size_t tone_sizes[] = { 788, 821, 823 };
	struct ums9117_audio_profile profile = {};
	u8 bytes[823];
	size_t i;

	check_invalid_fields(identity, sizeof(identity) / sizeof(identity[0]));
	make_profile_bytes(bytes);
	bytes[822] = 0;
	for (i = 0; i < sizeof(no_tone_sizes) / sizeof(no_tone_sizes[0]); i++)
		CHECK(parse(bytes, no_tone_sizes[i], false, &profile) == -22);
	for (i = 0; i < sizeof(tone_sizes) / sizeof(tone_sizes[0]); i++)
		CHECK(parse(bytes, tone_sizes[i], true, &profile) == -22);
	CHECK(ums9117_audio_profile_parse("fplinux,other-phone", bytes, 788,
					  false, &profile) == -22);
	CHECK(ums9117_audio_profile_parse("123456789012345678901234", bytes,
					  788, false, &profile) == -22);
}

static void test_gain_and_processing_safety(void)
{
	static const struct invalid_field gain_fields[] = {
		{ 32, 1, 0, false },   { 32, 8, 0, false },
		{ 55, 1, 0, false },   { 55, 8, 0, false },
		{ 33, 128, 0, false }, { 44, 128, 0, false },
		{ 56, 128, 0, false }, { 35, 121, 0, false },
		{ 45, 91, 0, false },  { 57, 101, 0, false },
		{ 42, 0, 1, true },    { 53, 0, 1, true },
	};
	static const struct invalid_field processing_fields[] = {
		{ 65, 0, 0, true },    { 306, 0, 128, true },
		{ 549, 2, 0, false },  { 90, 0, 0, true },
		{ 331, 0, 128, true }, { 582, 0, 64, true },
		{ 582, 0, 192, true }, { 96, 0, 64, true },
		{ 337, 0, 192, true },
	};
	struct ums9117_audio_profile profile = {};
	u8 bytes[822];
	const struct ums9117_audio_eq_section *last;

	check_invalid_fields(gain_fields,
			     sizeof(gain_fields) / sizeof(gain_fields[0]));
	check_invalid_fields(processing_fields,
			     sizeof(processing_fields) /
				     sizeof(processing_fields[0]));
	/* Signed coefficients at the final section retain their wire values. */
	make_profile_bytes(bytes);
	bytes[780] = 0x85;
	bytes[781] = 0xff;
	bytes[782] = 0x00;
	bytes[783] = 0x20;
	bytes[784] = 0xc8;
	bytes[785] = 0x01;
	bytes[786] = 0x00;
	bytes[787] = 0xf0;
	CHECK(parse(bytes, 788, false, &profile) == 0);
	last = &profile.combined_processing.section[2][5];
	CHECK(last->b1 == -123);
	CHECK(last->minus_a1 == 8192);
	CHECK(last->b2 == 456);
	CHECK(last->minus_a2 == -4096);
}

static void test_tone_capability(void)
{
	static const size_t silent_words[] = { 812, 814, 820 };
	struct ums9117_audio_profile profile;
	u8 bytes[822];
	size_t i;
	int expected = CONFIG_SND_UMS9117_SPEAKER_VIBRATOR ? -22 : 0;

	for (i = 0; i < 3; i++) {
		make_profile_bytes(bytes);
		/* Removing each rate's cosine destroys its unit-norm pair. */
		memset(bytes + 800 + i * 4, 0, 4);
		memset(&profile, 0, sizeof(profile));
		CHECK(parse(bytes, 822, true, &profile) == expected);
		CHECK(profile.fitted == !expected);
		CHECK(parse(bytes, 788, false, &profile) == 0);
	}
	for (i = 0; i < 3; i++) {
		make_profile_bytes(bytes);
		memset(bytes + silent_words[i], 0, 2);
		memset(&profile, 0, sizeof(profile));
		CHECK(parse(bytes, 822, true, &profile) == expected);
		CHECK(profile.fitted == !expected);
	}
}

/* Controlled kernel services; firmware storage expires at release. */
struct device {
	bool property_present;
	int property_error;
	int request_error;
	int compatible_error;
	u8 bytes[822];
	struct firmware firmware;
	unsigned int acquired;
	unsigned int released;
	bool firmware_live;
};

struct device_node {
	int unused;
};

static struct device_node root;
struct device_node *of_root = &root;
static struct device *active_device;

bool device_property_present(struct device *dev, const char *name)
{
	CHECK(!strcmp(name, "firmware-name"));
	return dev->property_present;
}

int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	CHECK(!strcmp(name, "firmware-name"));
	if (dev->property_error)
		return dev->property_error;
	*value = "test-audio.bin";
	return 0;
}

int request_firmware(const struct firmware **firmware, const char *name,
		     struct device *dev)
{
	CHECK(!strcmp(name, "test-audio.bin"));
	CHECK(!dev->firmware_live);
	if (dev->request_error)
		return dev->request_error;
	dev->firmware_live = true;
	dev->acquired++;
	*firmware = &dev->firmware;
	return 0;
}

void release_firmware(const struct firmware *firmware)
{
	CHECK(firmware == &active_device->firmware);
	CHECK(active_device->firmware_live);
	active_device->firmware_live = false;
	active_device->released++;
	memset(active_device->bytes, 0xa5, sizeof(active_device->bytes));
}

int of_property_read_string_index(const struct device_node *node,
				  const char *name, int index,
				  const char **value)
{
	CHECK(node == of_root);
	CHECK(!strcmp(name, "compatible"));
	CHECK(index == 0);
	CHECK(active_device->firmware_live);
	if (active_device->compatible_error)
		return active_device->compatible_error;
	*value = "fplinux,test-phone";
	return 0;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	CHECK(dev == active_device);
	CHECK(format != NULL);
	return error;
}

static void prepare_device(struct device *dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->property_present = true;
	make_profile_bytes(dev->bytes);
	dev->firmware.data = dev->bytes;
	dev->firmware.size = 788;
	active_device = dev;
}

static void check_firmware_balance(const struct device *dev,
				   unsigned int acquired)
{
	CHECK(dev->acquired == acquired);
	CHECK(dev->released == acquired);
	CHECK(!dev->firmware_live);
}

static void test_loader_fallback_and_errors(void)
{
	struct ums9117_audio_profile original;
	struct ums9117_audio_profile profile;
	struct device dev;
	size_t scenario;

	memset(&original, 0x5a, sizeof(original));
	original.fitted = false;
	for (scenario = 0; scenario < 2; scenario++) {
		prepare_device(&dev);
		if (!scenario)
			dev.property_present = false;
		else
			dev.request_error = -ENOENT;
		memcpy(&profile, &original, sizeof(profile));
		CHECK(ums9117_audio_profile_load(&dev, false, &profile) == 0);
		CHECK(!memcmp(&profile, &original, sizeof(profile)));
		check_firmware_balance(&dev, 0);
	}
	prepare_device(&dev);
	dev.property_error = -EINVAL;
	CHECK(ums9117_audio_profile_load(&dev, false, &profile) == -22);
	check_firmware_balance(&dev, 0);
	prepare_device(&dev);
	dev.request_error = -EAGAIN;
	CHECK(ums9117_audio_profile_load(&dev, false, &profile) == -11);
	check_firmware_balance(&dev, 0);
	prepare_device(&dev);
	dev.compatible_error = -ENODATA;
	CHECK(ums9117_audio_profile_load(&dev, false, &profile) == -61);
	check_firmware_balance(&dev, 1);
	prepare_device(&dev);
	dev.bytes[0] = 'X';
	CHECK(ums9117_audio_profile_load(&dev, false, &profile) == -22);
	check_firmware_balance(&dev, 1);
}

static void test_loader_copies_decoded_values(void)
{
	struct ums9117_audio_profile profile;
	struct device dev;

	prepare_device(&dev);
	memset(&profile, 0, sizeof(profile));
	CHECK(ums9117_audio_profile_load(&dev, false, &profile) == 0);
	check_firmware_balance(&dev, 1);
	check_decoded_profile(&profile);
	prepare_device(&dev);
	dev.firmware.size = 822;
	memset(&profile, 0, sizeof(profile));
	CHECK(ums9117_audio_profile_load(&dev, true, &profile) == 0);
	check_firmware_balance(&dev, 1);
	check_decoded_profile(&profile);
	if (CONFIG_SND_UMS9117_SPEAKER_VIBRATOR)
		check_decoded_tone(&profile.vibrate_tone);
}

int main(void)
{
	test_decoded_values();
	test_identity_and_length();
	test_gain_and_processing_safety();
	test_tone_capability();
	test_loader_fallback_and_errors();
	test_loader_copies_decoded_values();
	puts("audio-profile host parsing and firmware lifetime: ok");
	return 0;
}
