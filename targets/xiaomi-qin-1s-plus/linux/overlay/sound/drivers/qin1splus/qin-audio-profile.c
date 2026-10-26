// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "qin-audio-profile.h"

static s16 read_signed(const u8 **data)
{
	s16 value = (s16)get_unaligned_le16(*data);

	*data += sizeof(u16);
	return value;
}

int qin_audio_profile_parse(const u8 *bytes, size_t size,
			    struct qin_audio_profile *profile)
{
	struct qin_audio_profile parsed = {};
	struct sprd_vbc_eq6_section *section;
	const u8 *data;
	unsigned int i;

	if (size != QIN_AUDIO_PROFILE_BYTES || memcmp(bytes, "FPLQINAU", 8))
		return -EINVAL;
	parsed.processing_rate = get_unaligned_le32(bytes + 8);
	if (parsed.processing_rate != QIN_AUDIO_PROCESSING_RATE ||
	    bytes[12] > 2 || bytes[13] > 127 || bytes[14] != 1 ||
	    bytes[15] > 7 || bytes[16] > 127 || bytes[17] != 1 ||
	    bytes[18] != 1)
		return -EINVAL;
	parsed.dac_gain = bytes[12];
	parsed.digital_gain = bytes[13];
	parsed.digital_gain_enabled = bytes[14];
	parsed.mainmic_gain = bytes[15];
	parsed.limit = bytes[16];
	parsed.eq_enabled = bytes[17];
	parsed.alc_enabled = bytes[18];
	parsed.output_scale = get_unaligned_le16(bytes + 19);
	if (!parsed.output_scale || parsed.output_scale > 32767)
		return -EINVAL;
	data = bytes + 21;
	for (i = 0; i < QIN_AUDIO_ALC_WORD_COUNT; i++)
		parsed.alc[i] = read_signed(&data);
	for (i = 0; i < SPRD_VBC_EQ6_SECTION_COUNT; i++) {
		section = &parsed.section[i];
		section->scale = read_signed(&data);
		section->b0 = read_signed(&data);
		section->b1 = read_signed(&data);
		section->minus_a1 = read_signed(&data);
		section->b2 = read_signed(&data);
		section->minus_a2 = read_signed(&data);
		if (!sprd_vbc_eq6_section_valid(section))
			return -EINVAL;
	}
	*profile = parsed;
	return 0;
}

int qin_audio_profile_load(struct device *dev,
			   struct qin_audio_profile *profile)
{
	const struct firmware *firmware;
	const char *name;
	int ret;

	ret = device_property_read_string(dev, "firmware-name", &name);
	if (ret)
		return dev_err_probe(dev, ret, "missing audio profile name\n");
	ret = request_firmware(&firmware, name, dev);
	if (ret == -ENOENT) {
		dev_info(dev, "audio settings are not prepared\n");
		return -ENODEV;
	}
	if (ret)
		return dev_err_probe(dev, ret, "cannot load audio profile\n");
	ret = qin_audio_profile_parse(firmware->data, firmware->size, profile);
	release_firmware(firmware);
	if (ret)
		return dev_err_probe(dev, ret, "invalid audio profile\n");
	return 0;
}
