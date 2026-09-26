/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SC2720_CODEC_H
#define FPLINUX_UMS9117_SC2720_CODEC_H

#include <linux/bits.h>
#include <linux/types.h>

struct device;
struct ums9117_sc2720_codec;

#define UMS9117_SC2720_HEADPHONE_VOLUME_MAX 6U

enum ums9117_sc2720_capture_source {
	UMS9117_SC2720_CAPTURE_INTERNAL,
	UMS9117_SC2720_CAPTURE_HEADSET,
};

#define UMS9117_SC2720_OUTPUT_HEADPHONES BIT(0)
#define UMS9117_SC2720_OUTPUT_SPEAKER BIT(1)
#define UMS9117_SC2720_OUTPUT_MASK \
	(UMS9117_SC2720_OUTPUT_HEADPHONES | UMS9117_SC2720_OUTPUT_SPEAKER)

/* The PCM owner serializes every operation and quiesces before devm release. */
struct ums9117_sc2720_codec *ums9117_sc2720_codec_create(struct device *dev);
/*
 * Power the headset detector that drives the jack insert EIC. Call it before
 * the first prepare: playback and capture then save and restore its supply
 * as they find it. It stays powered until the PMIC powers off.
 */
int ums9117_sc2720_codec_enable_jack_detect(struct ums9117_sc2720_codec *codec);
void ums9117_sc2720_codec_get_volume(struct ums9117_sc2720_codec *codec,
				     unsigned int *left, unsigned int *right);
int ums9117_sc2720_codec_set_volume(struct ums9117_sc2720_codec *codec,
				    unsigned int left, unsigned int right);
/*
 * Playback opens the requested outputs, and the PA word configures the
 * speaker amplifier. An empty set runs the DAC without an output. A prepared
 * codec takes a changed set or word as set_outputs does.
 */
int ums9117_sc2720_codec_prepare(struct ums9117_sc2720_codec *codec,
				 unsigned int outputs, u16 pa_word);
/*
 * Enabled playback closes outputs that left the set and opens those that
 * joined it; an open PA reopens with a changed word.
 */
int ums9117_sc2720_codec_set_outputs(struct ums9117_sc2720_codec *codec,
				     unsigned int outputs, u16 pa_word);
/* A muted speaker has its PA disabled; transitions may sleep for 30 ms. */
int ums9117_sc2720_codec_set_speaker_mute(struct ums9117_sc2720_codec *codec,
					  bool mute);
/*
 * Keep the PA open for the vibrate tone, with or without the speaker output.
 * Open headphones stay muted until vibration ends.
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
