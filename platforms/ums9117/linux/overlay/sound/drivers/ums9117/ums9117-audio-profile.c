// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ums9117-audio-profile.h"

#define UMS9117_AUDIO_PROFILE_MAGIC_SIZE 8U
#define UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE 24U
/*
 * The header is followed by the speaker section, the combined headphone and
 * speaker section, the playback processing and, on boards with speaker
 * vibration, the vibrate tone. Each output section starts with its PA word;
 * the combined section then adds its headphone PGA level before the gains.
 */
#define UMS9117_AUDIO_PROFILE_SPEAKER_BYTES \
	(sizeof(u16) + UMS9117_AUDIO_PROFILE_LEVEL_COUNT)
#define UMS9117_AUDIO_PROFILE_COMBINED_BYTES \
	(sizeof(u16) + sizeof(u8) + UMS9117_AUDIO_PROFILE_LEVEL_COUNT)
/*
 * The processing holds the headphone, speaker and combined routes in that
 * order. Each has S6, the ALC enable and its ALC words, then for each DAC rate
 * six EQ6 sections of scale, B0, B1, -A1, B2 and -A2, all little-endian.
 */
#define UMS9117_AUDIO_PROFILE_EQ_SECTION_BYTES (6 * sizeof(u16))
#define UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES                     \
	(sizeof(u16) + sizeof(u8) +                                      \
	 UMS9117_AUDIO_ALC_WORD_COUNT * sizeof(u16) +                    \
	 UMS9117_AUDIO_DAC_RATE_COUNT * UMS9117_AUDIO_EQ_SECTION_COUNT * \
		 UMS9117_AUDIO_PROFILE_EQ_SECTION_BYTES)
#define UMS9117_AUDIO_PROFILE_PROCESSING_BYTES \
	(3 * UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES)
/* Oscillator pairs for each DAC rate, then level, fall, rise and hold. */
#define UMS9117_AUDIO_PROFILE_VIBRATE_TONE_BYTES \
	(2 * UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32) + 5 * sizeof(u16))
/* Q30 rotation pairs are rounded to one part in 2^30 of unit norm. */
#define UMS9117_AUDIO_PROFILE_VIBRATE_NORM BIT_ULL(60)
#define UMS9117_AUDIO_PROFILE_VIBRATE_NORM_TOLERANCE BIT_ULL(40)
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN 2U
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX 7U
#define UMS9117_AUDIO_PROFILE_PA_WORD_MAX 0xffU
#define UMS9117_AUDIO_PROFILE_DAC_GAIN_MAX 127U

static const u8 ums9117_audio_profile_magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE] = {
	'F', 'P', 'A', 'U', 'D', 'I', 'O', '\0'
};

struct ums9117_audio_profile_data {
	u8 magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE];
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE];
	u8 headphone_pga;
	u8 dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
};

static int parse_vibrate_tone(const u8 *data,
			      struct ums9117_audio_vibrate_tone *tone)
{
	const u8 *cos_data = data + UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32);
	const u8 *envelope =
		cos_data + UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32);
	unsigned int i;

	for (i = 0; i < UMS9117_AUDIO_DAC_RATE_COUNT; i++) {
		s64 sin = (s32)get_unaligned_le32(data + i * sizeof(u32));
		s64 cos = (s32)get_unaligned_le32(cos_data + i * sizeof(u32));
		s64 norm = sin * sin + cos * cos;

		/* Any other norm makes the recursive oscillator decay or grow. */
		if (abs(norm - (s64)UMS9117_AUDIO_PROFILE_VIBRATE_NORM) >
		    (s64)UMS9117_AUDIO_PROFILE_VIBRATE_NORM_TOLERANCE)
			return -EINVAL;
		tone->sin[i] = (u32)sin;
		tone->cos[i] = (u32)cos;
	}
	tone->level[0] = get_unaligned_le16(envelope);
	tone->level[1] = get_unaligned_le16(envelope + 2);
	tone->fall = get_unaligned_le16(envelope + 4);
	tone->rise = get_unaligned_le16(envelope + 6);
	tone->hold = get_unaligned_le16(envelope + 8);
	/* A zero level is silent and a zero hold never starts the tone. */
	if (!tone->level[0] || !tone->level[1] || !tone->hold)
		return -EINVAL;
	return 0;
}

/* A larger DG code attenuates more; a louder level never has a larger one. */
static int parse_dac_gains(const u8 *data, u8 *gains)
{
	unsigned int i;

	for (i = 0; i < UMS9117_AUDIO_PROFILE_LEVEL_COUNT; i++) {
		if (data[i] > UMS9117_AUDIO_PROFILE_DAC_GAIN_MAX ||
		    (i && data[i - 1] < data[i]))
			return -EINVAL;
		gains[i] = data[i];
	}
	return 0;
}

static bool headphone_pga_valid(u8 pga)
{
	return pga >= UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN &&
	       pga <= UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX;
}

static s16 read_s16(const u8 **data)
{
	s16 value = (s16)get_unaligned_le16(*data);

	*data += sizeof(u16);
	return value;
}

/*
 * An unstable section drives a full-scale DC or fs/2 square wave into the
 * speaker amplifier. With A0 as unity the quantized poles lie inside the unit
 * circle when |a2| < 1 and |a1| < 1 + a2.
 */
static bool eq_section_valid(const struct ums9117_audio_eq_section *section)
{
	int minus_a1 = section->minus_a1;
	int minus_a2 = section->minus_a2;

	return section->scale > 0 && abs(minus_a2) < UMS9117_AUDIO_EQ_A0 &&
	       abs(minus_a1) < UMS9117_AUDIO_EQ_A0 - minus_a2;
}

static int parse_dac_processing(const u8 *data,
				struct ums9117_audio_dac_processing *processing)
{
	struct ums9117_audio_eq_section *section;
	unsigned int rate;
	unsigned int i;
	u8 alc_enabled;

	processing->output_scale = get_unaligned_le16(data);
	if (!processing->output_scale || processing->output_scale > S16_MAX)
		return -EINVAL;
	data += sizeof(u16);
	alc_enabled = *data++;
	if (alc_enabled > 1)
		return -EINVAL;
	processing->alc_enabled = alc_enabled;
	for (i = 0; i < UMS9117_AUDIO_ALC_WORD_COUNT; i++)
		processing->alc[i] = read_s16(&data);
	for (rate = 0; rate < UMS9117_AUDIO_DAC_RATE_COUNT; rate++) {
		for (i = 0; i < UMS9117_AUDIO_EQ_SECTION_COUNT; i++) {
			section = &processing->section[rate][i];
			section->scale = read_s16(&data);
			section->b0 = read_s16(&data);
			section->b1 = read_s16(&data);
			section->minus_a1 = read_s16(&data);
			section->b2 = read_s16(&data);
			section->minus_a2 = read_s16(&data);
			if (!eq_section_valid(section))
				return -EINVAL;
		}
	}
	return 0;
}

int ums9117_audio_profile_parse(const char *machine_compatible, const u8 *bytes,
				size_t size, bool speaker_vibration,
				struct ums9117_audio_profile *profile)
{
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE] = {};
	const struct ums9117_audio_profile_data *data;
	const u8 *processing;
	const u8 *combined;
	const u8 *speaker;
	size_t compatible_length;
	size_t expected_size;
	u8 combined_pga;
	int ret;

	expected_size = sizeof(*data) + UMS9117_AUDIO_PROFILE_SPEAKER_BYTES +
			UMS9117_AUDIO_PROFILE_COMBINED_BYTES +
			UMS9117_AUDIO_PROFILE_PROCESSING_BYTES;
	if (speaker_vibration)
		expected_size += UMS9117_AUDIO_PROFILE_VIBRATE_TONE_BYTES;
	if (size != expected_size)
		return -EINVAL;
	data = (const struct ums9117_audio_profile_data *)bytes;
	if (memcmp(data->magic, ums9117_audio_profile_magic,
		   UMS9117_AUDIO_PROFILE_MAGIC_SIZE))
		return -EINVAL;
	compatible_length = strlen(machine_compatible);
	if (compatible_length >= sizeof(compatible))
		return -EINVAL;
	memcpy(compatible, machine_compatible, compatible_length);
	if (memcmp(data->compatible, compatible, sizeof(compatible)))
		return -EINVAL;
	if (!headphone_pga_valid(data->headphone_pga))
		return -EINVAL;
	ret = parse_dac_gains(data->dac_gain, profile->dac_gain);
	if (ret)
		return ret;
	profile->codec_volume = data->headphone_pga - 1U;

	speaker = bytes + sizeof(*data);
	profile->speaker_pa_word = get_unaligned_le16(speaker);
	if (profile->speaker_pa_word > UMS9117_AUDIO_PROFILE_PA_WORD_MAX)
		return -EINVAL;
	ret = parse_dac_gains(speaker + sizeof(u16), profile->speaker_dac_gain);
	if (ret)
		return ret;

	combined = speaker + UMS9117_AUDIO_PROFILE_SPEAKER_BYTES;
	profile->combined_pa_word = get_unaligned_le16(combined);
	if (profile->combined_pa_word > UMS9117_AUDIO_PROFILE_PA_WORD_MAX)
		return -EINVAL;
	combined_pga = combined[sizeof(u16)];
	if (!headphone_pga_valid(combined_pga))
		return -EINVAL;
	profile->combined_codec_volume = combined_pga - 1U;
	ret = parse_dac_gains(combined + sizeof(u16) + sizeof(u8),
			      profile->combined_dac_gain);
	if (ret)
		return ret;

	processing = combined + UMS9117_AUDIO_PROFILE_COMBINED_BYTES;
	ret = parse_dac_processing(processing, &profile->headphone_processing);
	if (ret)
		return ret;
	ret = parse_dac_processing(
		processing + UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES,
		&profile->speaker_processing);
	if (ret)
		return ret;
	ret = parse_dac_processing(
		processing + 2 * UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES,
		&profile->combined_processing);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_SND_UMS9117_SPEAKER_VIBRATOR) &&
	    speaker_vibration) {
		ret = parse_vibrate_tone(
			processing + UMS9117_AUDIO_PROFILE_PROCESSING_BYTES,
			&profile->vibrate_tone);
		if (ret)
			return ret;
	}
	profile->fitted = true;
	return 0;
}

int ums9117_audio_profile_load(struct device *dev, bool speaker_vibration,
			       struct ums9117_audio_profile *profile)
{
	const struct firmware *firmware;
	const char *machine_compatible;
	const char *firmware_name;
	int ret;

	if (!device_property_present(dev, "firmware-name"))
		return 0;
	ret = device_property_read_string(dev, "firmware-name", &firmware_name);
	if (ret)
		return dev_err_probe(dev, ret, "invalid audio firmware name\n");
	ret = request_firmware(&firmware, firmware_name, dev);
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return dev_err_probe(dev, ret, "cannot load audio profile\n");

	ret = of_property_read_string_index(of_root, "compatible", 0,
					    &machine_compatible);
	if (ret) {
		release_firmware(firmware);
		return dev_err_probe(dev, ret,
				     "cannot read machine compatible\n");
	}
	ret = ums9117_audio_profile_parse(machine_compatible, firmware->data,
					  firmware->size, speaker_vibration,
					  profile);
	release_firmware(firmware);
	if (ret)
		return dev_err_probe(dev, ret, "invalid audio profile\n");
	return 0;
}
