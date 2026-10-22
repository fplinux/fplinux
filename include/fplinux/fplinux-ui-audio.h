/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_AUDIO_H
#define FPLINUX_UI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_ui;
struct fplinux_audio_stream;

/*
 * Streams carry interleaved signed 16-bit native-endian mono or stereo
 * frames. The system resamples and mixes them without changing volume or
 * output routes. Applications never open the sound device.
 *
 * Hiding an application (Home, keyguard, display off or a system notice)
 * pauses its streams with their frames kept; showing it resumes them.
 * Exiting, CLOSE, a crash and session shutdown end them.
 * A busy output, including FM playback, holds queued frames until available.
 * Call these functions on the thread that called fplinux_ui_open();
 * fplinux_ui_close() frees all streams still open.
 */
#define FPLINUX_UI_AUDIO_RATE_MIN 8000
#define FPLINUX_UI_AUDIO_RATE_MAX 48000
#define FPLINUX_UI_AUDIO_STREAMS_MAX 4

/* Configured playback capability, fixed for the process. */
bool fplinux_ui_audio_supported(const struct fplinux_ui *ui);
/* Whether this process may open and play streams while hidden. */
bool fplinux_ui_audio_background_supported(const struct fplinux_ui *ui);

/*
 * Returns NULL with errno, in this order: EINVAL for a rate outside the
 * range above or channels other than 1 or 2; ENOTSUP without sound; EAGAIN
 * while hidden or leaving; EMFILE with four streams open; EBUSY if the
 * system has no free application stream; ECONNREFUSED without the service;
 * ETIMEDOUT if it does not answer within 500 ms, or a connection error.
 */
struct fplinux_audio_stream *fplinux_ui_audio_open(struct fplinux_ui *ui,
						   unsigned int rate,
						   unsigned int channels);
/*
 * Copies up to count frames without blocking, fewer when the ring is full.
 * A ring holds at least 250 ms. Returns -1 with EINVAL for NULL frames with
 * nonzero count, or EPIPE after the service ends the stream; open a new one.
 */
long fplinux_ui_audio_write(struct fplinux_audio_stream *stream,
			    const int16_t *frames, size_t count);
/* Frames pending, including the output queue; -1 with EPIPE once ended. */
long fplinux_ui_audio_queued(const struct fplinux_audio_stream *stream);
/* Drops frames still in this stream. An already mixed output tail is bounded
 * by the system's output buffer. NULL is ignored.
 */
void fplinux_ui_audio_close(struct fplinux_audio_stream *stream);

#endif
