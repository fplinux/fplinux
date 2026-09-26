/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_JACK_H
#define FPLINUX_UMS9117_JACK_H

struct device;
struct snd_card;
struct ums9117_jack;
struct ums9117_sc2720_codec;

/*
 * Add the Headphone Jack control and switch input device to an unregistered
 * card and report the current state, or return NULL for a device without a
 * hp-det line. Create the jack before the codec's first prepare. Every other
 * function accepts that NULL.
 */
struct ums9117_jack *ums9117_jack_create(struct device *dev,
					 struct snd_card *card,
					 struct ums9117_sc2720_codec *codec);
/* Report later changes; stop before the card is disconnected or freed. */
void ums9117_jack_start(struct ums9117_jack *jack);
void ums9117_jack_stop(struct ums9117_jack *jack);
/* Without wakeup-source, a change during sleep is reported on resume. */
int ums9117_jack_suspend(struct ums9117_jack *jack);
void ums9117_jack_resume(struct ums9117_jack *jack);

#endif
