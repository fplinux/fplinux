/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_AUDIO_PROFILE_H
#define FPLINUX_UMS9117_AUDIO_PROFILE_H

#include <linux/types.h>

#include "ums9117-audio.h"

struct device;

#define UMS9117_AUDIO_PROFILE_LEVEL_COUNT 9U

struct ums9117_audio_profile {
	u8 dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	u8 speaker_dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	/*
	 * Both outputs share one DAC gain at a fixed headphone level. The
	 * halved PCM samples make the headphones 6 dB quieter in this mode.
	 */
	u8 combined_dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	struct ums9117_audio_dac_processing headphone_processing;
	struct ums9117_audio_dac_processing speaker_processing;
	struct ums9117_audio_dac_processing combined_processing;
	struct ums9117_audio_vibrate_tone vibrate_tone;
	u16 speaker_pa_word;
	u16 combined_pa_word;
	u8 codec_volume;
	u8 combined_codec_volume;
	bool fitted;
};

int ums9117_audio_profile_parse(const char *machine_compatible, const u8 *bytes,
				size_t size, bool speaker_vibration,
				struct ums9117_audio_profile *profile);
int ums9117_audio_profile_load(struct device *dev, bool speaker_vibration,
			       struct ums9117_audio_profile *profile);

#endif
