/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_AUDIO_PROFILE_H
#define FPLINUX_QIN_AUDIO_PROFILE_H

#include <linux/types.h>
#include <linux/soc/sprd/vbc-eq6.h>

#define QIN_AUDIO_PROFILE_BYTES 115U
#define QIN_AUDIO_PROCESSING_RATE 44100U
#define QIN_AUDIO_ALC_WORD_COUNT 11U
#define QIN_AUDIO_EQ6_WORD_COUNT 86U

struct device;

struct qin_audio_profile {
	u32 processing_rate;
	u8 dac_gain;
	u8 digital_gain;
	bool digital_gain_enabled;
	u8 mainmic_gain;
	u8 limit;
	bool eq_enabled;
	bool alc_enabled;
	u16 output_scale;
	s16 alc[QIN_AUDIO_ALC_WORD_COUNT];
	struct sprd_vbc_eq6_section section[SPRD_VBC_EQ6_SECTION_COUNT];
};

/* Register contents outlive a stream until their restoration succeeds. */
struct qin_audio_processing_state {
	u32 coefficients[QIN_AUDIO_EQ6_WORD_COUNT];
	u32 alc[QIN_AUDIO_ALC_WORD_COUNT];
	bool active;
};

int qin_audio_profile_parse(const u8 *bytes, size_t size,
			    struct qin_audio_profile *profile);
int qin_audio_profile_load(struct device *dev,
			   struct qin_audio_profile *profile);
int qin_audio_processing_prepare(struct device *dev, void __iomem *vbc,
				 const struct qin_audio_profile *profile,
				 struct qin_audio_processing_state *state);
int qin_audio_processing_restore(struct device *dev, void __iomem *vbc,
				 struct qin_audio_processing_state *state);

#endif
