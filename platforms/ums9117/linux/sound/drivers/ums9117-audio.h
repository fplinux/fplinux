/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_AUDIO_H
#define FPLINUX_UMS9117_AUDIO_H

#include <linux/types.h>

struct platform_device;
struct ums9117_audio;

#define UMS9117_AUDIO_FIFO_FRAMES 320U

struct ums9117_audio *ums9117_audio_create(struct platform_device *pdev);
void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left_gain,
				u8 right_gain);
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
