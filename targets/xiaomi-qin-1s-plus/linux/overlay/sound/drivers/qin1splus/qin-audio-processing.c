// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/soc/sprd/vbc-eq6-io.h>

#include "qin-audio-profile.h"

#define QIN_VBC_CLEAR 0x00
#define QIN_VBC_PROCESSING 0x48
#define QIN_VBC_ALC(n) (0x4c + 4 * (n))
#define QIN_VBC_EQ6_ENABLE BIT(9)
#define QIN_VBC_ALC_ENABLE BIT(11)
#define QIN_VBC_EQ6_CLEAR BIT(10)
#define QIN_VBC_ALC_CLEAR BIT(11)
#define QIN_VBC_ACCUMULATOR_CLEAR GENMASK(15, 14)

static int update_processing(void __iomem *vbc, u32 mask, u32 value)
{
	u32 previous = readl(vbc + QIN_VBC_PROCESSING);
	u32 word = (previous & ~(mask | QIN_VBC_ACCUMULATOR_CLEAR)) | value;

	writel(word, vbc + QIN_VBC_PROCESSING);
	return (readl(vbc + QIN_VBC_PROCESSING) & mask) == value ? 0 : -EIO;
}

static void write_scale(void __iomem *vbc, unsigned int coefficient, u16 value)
{
	writel(value, vbc + SPRD_VBC_EQ6_COEFFICIENT_HIGH(coefficient));
	writel(0, vbc + SPRD_VBC_EQ6_COEFFICIENT_LOW(coefficient));
}

static void mute_scales(void __iomem *vbc)
{
	unsigned int i;

	for (i = 0; i < SPRD_VBC_EQ6_SECTION_COUNT; i++)
		write_scale(vbc, i * SPRD_VBC_EQ6_SECTION_COEFFICIENTS, 0);
	write_scale(vbc, SPRD_VBC_EQ6_OUTPUT_SCALE, 0);
}

int qin_audio_processing_prepare(struct device *dev, void __iomem *vbc,
				 const struct qin_audio_profile *profile,
				 struct qin_audio_processing_state *state)
{
	struct sprd_vbc_eq6_mismatch mismatch;
	u32 enable = (profile->eq_enabled ? QIN_VBC_EQ6_ENABLE : 0) |
		     (profile->alc_enabled ? QIN_VBC_ALC_ENABLE : 0);
	unsigned int i;
	int ret;

	if (state->active)
		return -EBUSY;
	for (i = 0; i < QIN_AUDIO_EQ6_WORD_COUNT; i++)
		state->coefficients[i] = readl(vbc + 0x100 + 4 * i);
	for (i = 0; i < QIN_AUDIO_ALC_WORD_COUNT; i++)
		state->alc[i] = readl(vbc + QIN_VBC_ALC(i));
	state->active = true;
	/* The DAC is stopped and both analog outputs are muted by the caller. */
	mute_scales(vbc);
	ret = update_processing(vbc,
				0xff | QIN_VBC_EQ6_ENABLE | QIN_VBC_ALC_ENABLE,
				profile->limit | enable);
	if (ret)
		return ret;
	writel(QIN_VBC_EQ6_CLEAR | QIN_VBC_ALC_CLEAR, vbc + QIN_VBC_CLEAR);
	ret = sprd_vbc_eq6_write_filters(vbc, profile->section, &mismatch);
	if (ret) {
		dev_err(dev, "EQ6 coefficient %u reads %#x, expected %#x\n",
			mismatch.coefficient, mismatch.observed,
			mismatch.expected);
		return ret;
	}
	for (i = 0; i < QIN_AUDIO_ALC_WORD_COUNT; i++) {
		/* Preserve the signed parameter's complete hardware word. */
		writel((u32)(s32)profile->alc[i], vbc + QIN_VBC_ALC(i));
		if ((readl(vbc + QIN_VBC_ALC(i)) & 0xffff) !=
		    (u16)profile->alc[i])
			return -EIO;
	}
	write_scale(vbc, SPRD_VBC_EQ6_OUTPUT_SCALE, profile->output_scale);
	for (i = 0; i < SPRD_VBC_EQ6_SECTION_COUNT; i++)
		write_scale(vbc, i * SPRD_VBC_EQ6_SECTION_COEFFICIENTS,
			    profile->section[i].scale);
	return 0;
}

int qin_audio_processing_restore(struct device *dev, void __iomem *vbc,
				 struct qin_audio_processing_state *state)
{
	unsigned int i;
	int ret;

	if (!state->active)
		return 0;
	mute_scales(vbc);
	ret = update_processing(vbc, QIN_VBC_EQ6_ENABLE | QIN_VBC_ALC_ENABLE,
				0);
	if (ret)
		return ret;
	writel(QIN_VBC_EQ6_CLEAR | QIN_VBC_ALC_CLEAR, vbc + QIN_VBC_CLEAR);
	for (i = 0; i < QIN_AUDIO_EQ6_WORD_COUNT; i++)
		writel(state->coefficients[i], vbc + 0x100 + 4 * i);
	for (i = 0; i < QIN_AUDIO_ALC_WORD_COUNT; i++)
		writel(state->alc[i], vbc + QIN_VBC_ALC(i));
	for (i = 0; i < QIN_AUDIO_EQ6_WORD_COUNT; i++) {
		if (readl(vbc + 0x100 + 4 * i) != state->coefficients[i]) {
			dev_err(dev, "cannot restore EQ6 word %u\n", i);
			return -EIO;
		}
	}
	for (i = 0; i < QIN_AUDIO_ALC_WORD_COUNT; i++) {
		if (readl(vbc + QIN_VBC_ALC(i)) != state->alc[i]) {
			dev_err(dev, "cannot restore ALC word %u\n", i);
			return -EIO;
		}
	}
	state->active = false;
	return 0;
}
