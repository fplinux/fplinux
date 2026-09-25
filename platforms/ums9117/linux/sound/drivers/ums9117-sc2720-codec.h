/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SC2720_CODEC_H
#define FPLINUX_UMS9117_SC2720_CODEC_H

#include <linux/types.h>

struct device;
struct ums9117_sc2720_codec;

#define UMS9117_SC2720_HEADPHONE_VOLUME_MAX 6U

enum ums9117_sc2720_capture_source {
	UMS9117_SC2720_CAPTURE_INTERNAL,
	UMS9117_SC2720_CAPTURE_HEADSET,
};

enum ums9117_sc2720_playback_output {
	UMS9117_SC2720_OUTPUT_HEADPHONES,
	UMS9117_SC2720_OUTPUT_SPEAKER,
};

/* The PCM owner serializes every operation and quiesces before devm release. */
struct ums9117_sc2720_codec *ums9117_sc2720_codec_create(struct device *dev);
void ums9117_sc2720_codec_get_volume(struct ums9117_sc2720_codec *codec,
				     unsigned int *left, unsigned int *right);
int ums9117_sc2720_codec_set_volume(struct ums9117_sc2720_codec *codec,
				    unsigned int left, unsigned int right);
int ums9117_sc2720_codec_prepare(struct ums9117_sc2720_codec *codec,
				 enum ums9117_sc2720_playback_output output,
				 u16 speaker_pa_word);
/* A muted speaker has its PA disabled; transitions may sleep for 30 ms. */
int ums9117_sc2720_codec_set_speaker_mute(struct ums9117_sc2720_codec *codec,
					  bool mute);
/*
 * Keep the PA open for the vibrate tone. With headphones the PA opens beside
 * them and their volume stays muted until vibration ends.
 */
int ums9117_sc2720_codec_set_vibration(struct ums9117_sc2720_codec *codec,
				       bool vibration);
int ums9117_sc2720_codec_enable(struct ums9117_sc2720_codec *codec);
int ums9117_sc2720_codec_prepare_capture(
	struct ums9117_sc2720_codec *codec,
	enum ums9117_sc2720_capture_source source);
int ums9117_sc2720_codec_enable_capture(struct ums9117_sc2720_codec *codec);
/* Stop the ADC producer before stopping the digital capture receiver. */
int ums9117_sc2720_codec_stop_capture(struct ums9117_sc2720_codec *codec);
int ums9117_sc2720_codec_disable_capture(struct ums9117_sc2720_codec *codec);
/* Playback stop retains calibration; disable releases its prepared state. */
int ums9117_sc2720_codec_stop(struct ums9117_sc2720_codec *codec);
/* Each direction leaves the other prepared direction powered. */
int ums9117_sc2720_codec_disable(struct ums9117_sc2720_codec *codec);

#endif
