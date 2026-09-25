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

struct ums9117_audio *ums9117_audio_create(struct platform_device *pdev);
void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left_gain,
				u8 right_gain);
/* Set once before the first prepare; every DAC prepare programs the tone. */
void ums9117_audio_set_vibrate_tone(
	struct ums9117_audio *audio,
	const struct ums9117_audio_vibrate_tone *tone);
/* The tone follows the DAC: a later prepare restores the requested state. */
void ums9117_audio_set_vibration(struct ums9117_audio *audio, bool on);
/* Hold the DAC gain at its minimum and mute FM without changing the tone. */
void ums9117_audio_set_music_mute(struct ums9117_audio *audio, bool mute);
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
