/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_AUDIO_H
#define FPLINUX_UMS9117_AUDIO_H

#include <linux/types.h>

struct platform_device;
struct ums9117_audio;

#define UMS9117_AUDIO_FIFO_FRAMES 320U

enum ums9117_audio_dac_rate {
	UMS9117_AUDIO_DAC_24K,
	UMS9117_AUDIO_DAC_32K,
	UMS9117_AUDIO_DAC_48K,
	UMS9117_AUDIO_DAC_RATE_COUNT,
};

/* Fitted VBC vibrate-tone oscillator and envelope, in register order. */
struct ums9117_audio_vibrate_tone {
	u32 sin[UMS9117_AUDIO_DAC_RATE_COUNT];
	u32 cos[UMS9117_AUDIO_DAC_RATE_COUNT];
	u16 level[2];
	u16 fall;
	u16 rise;
	u16 hold;
};

#define UMS9117_AUDIO_EQ_SECTION_COUNT 6U
#define UMS9117_AUDIO_ALC_WORD_COUNT 11U
/* Q14 unity: the leading denominator coefficient of every EQ6 section. */
#define UMS9117_AUDIO_EQ_A0 16384

/*
 * One EQ6 biquad: Q12 input scale and Q14 coefficients. The denominator
 * coefficients are stored negated, as the VBC registers take them.
 */
struct ums9117_audio_eq_section {
	s16 scale;
	s16 b0;
	s16 b1;
	s16 minus_a1;
	s16 b2;
	s16 minus_a2;
};

/* Stock DAC processing of one output route: EQ6 per DAC rate, S6 and ALC. */
struct ums9117_audio_dac_processing {
	struct ums9117_audio_eq_section section[UMS9117_AUDIO_DAC_RATE_COUNT]
					       [UMS9117_AUDIO_EQ_SECTION_COUNT];
	s16 alc[UMS9117_AUDIO_ALC_WORD_COUNT];
	u16 output_scale;
	bool alc_enabled;
};

struct ums9117_audio *ums9117_audio_create(struct platform_device *pdev);
/* FM playback follows the same left and right gain codes. */
void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left_gain,
				u8 right_gain);
/* Set once before the first prepare; every DAC prepare programs the tone. */
void ums9117_audio_set_vibrate_tone(
	struct ums9117_audio *audio,
	const struct ums9117_audio_vibrate_tone *tone);
/* The tone follows the DAC: a later prepare restores the requested state. */
void ums9117_audio_set_vibration(struct ums9117_audio *audio, bool on);
/* Hold the DAC and FM gains at their minimum and mute FM, but not the tone. */
void ums9117_audio_set_music_mute(struct ums9117_audio *audio, bool mute);
/*
 * Select the route processing for PCM and FM playback, or NULL for none. eq
 * runs its EQ6 and output scale; alc runs its ALC when the route enables ALC.
 * Each block is bypassed otherwise. The caller keeps the processing alive
 * while it is selected. A prepared DAC changes at once, fading EQ6 while it
 * runs, and the next prepare programs the selection otherwise. On error EQ6
 * is off.
 */
int ums9117_audio_set_dac_processing(
	struct ums9117_audio *audio,
	const struct ums9117_audio_dac_processing *processing, bool eq,
	bool alc);
int ums9117_audio_prepare(struct ums9117_audio *audio, unsigned int rate);
int ums9117_audio_prepare_fm(struct ums9117_audio *audio);
int ums9117_audio_prepare_capture(struct ums9117_audio *audio);
void ums9117_audio_start_capture(struct ums9117_audio *audio);
void ums9117_audio_stop_capture(struct ums9117_audio *audio);
void ums9117_audio_release_capture(struct ums9117_audio *audio);
int ums9117_audio_capture_available(struct ums9117_audio *audio);
u16 ums9117_audio_read_capture(struct ums9117_audio *audio);
void ums9117_audio_report_capture(struct ums9117_audio *audio,
				  const char *reason);
void ums9117_audio_start(struct ums9117_audio *audio);
void ums9117_audio_stop(struct ums9117_audio *audio);
void ums9117_audio_release(struct ums9117_audio *audio);
/* Maximum of the two lane occupancies, or a negative error. */
int ums9117_audio_queued(struct ums9117_audio *audio);
void ums9117_audio_write(struct ums9117_audio *audio, u16 left, u16 right);

#endif
