/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_RUNTIME_MEDIA_H
#define FPLINUX_RUNTIME_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FPLINUX_RUNTIME_MEDIA_HANDLES 16U
#define FPLINUX_RUNTIME_MEDIA_BYTES (16U * 1024U * 1024U)

struct fplinux_ui;
struct fplinux_runtime_media;

enum fplinux_runtime_media_format {
	FPLINUX_RUNTIME_MEDIA_AUTO,
	FPLINUX_RUNTIME_MEDIA_WAV,
	FPLINUX_RUNTIME_MEDIA_MP3,
	FPLINUX_RUNTIME_MEDIA_MIDI,
	FPLINUX_RUNTIME_MEDIA_TONE_SEQUENCE,
	FPLINUX_RUNTIME_MEDIA_TONE,
};

enum fplinux_runtime_media_command {
	FPLINUX_RUNTIME_MEDIA_PREPARE,
	FPLINUX_RUNTIME_MEDIA_PREFETCH,
	FPLINUX_RUNTIME_MEDIA_START,
	FPLINUX_RUNTIME_MEDIA_PAUSE,
	FPLINUX_RUNTIME_MEDIA_RELEASE,
	FPLINUX_RUNTIME_MEDIA_SEEK,
	FPLINUX_RUNTIME_MEDIA_GAIN,
};

enum fplinux_runtime_media_event_type {
	FPLINUX_RUNTIME_MEDIA_RESULT,
	FPLINUX_RUNTIME_MEDIA_END,
	FPLINUX_RUNTIME_MEDIA_ERROR,
};

struct fplinux_runtime_media_info {
	enum fplinux_runtime_media_format format;
	unsigned int rate;
	unsigned int channels;
	uint64_t position_us;
	uint64_t duration_us;
	bool duration_known;
	bool playing;
};

struct fplinux_runtime_media_event {
	uint32_t handle;
	enum fplinux_runtime_media_event_type type;
	enum fplinux_runtime_media_command command;
	int error;
	struct fplinux_runtime_media_info info;
};

typedef void (*fplinux_runtime_media_event_fn)(
	const struct fplinux_runtime_media_event *event, void *context);

/*
 * All entry points and callbacks run on the UI owner thread. One private
 * worker owns every decoder and never calls UI or guest APIs. Prepared handles
 * consume actual encoded bytes/events, not a reserved maximum-sized buffer.
 * There are at most sixteen handles and four active UI streams, at their native
 * formats; the system owns mixing/resampling. The callback queues guest events
 * and schedules its normal dispatch, without calling the guest directly.
 */
struct fplinux_runtime_media *
fplinux_runtime_media_open(struct fplinux_ui *ui,
			   fplinux_runtime_media_event_fn callback,
			   void *context);
void fplinux_runtime_media_close(struct fplinux_runtime_media *media);

/*
 * Copies at most BYTES in total across pending uploads/encoded resources.
 * A nonzero opaque handle starts PREPARE; its RESULT arrives asynchronously.
 * Explicit formats are checked against the decoded content. TONE uses note
 * 0..127 and duration_ms > 0, and ignores bytes/count. Other formats use bytes.
 */
uint32_t fplinux_runtime_media_create(struct fplinux_runtime_media *media,
				      enum fplinux_runtime_media_format format,
				      const void *bytes, size_t count,
				      unsigned int note, uint64_t duration_ms);
/* Drops the handle immediately; a worker finishing an old request cannot
 * deliver events or PCM for it. Decoder cleanup completes before slot reuse. */
void fplinux_runtime_media_destroy(struct fplinux_runtime_media *media,
				   uint32_t handle);

/*
 * At most one command may be pending per handle (EBUSY otherwise). START only
 * succeeds after decoder preparation and a real UI stream are available.
 * PAUSE freezes heard time; MIDI drops future queued PCM and retains its own
 * prefetched synth and the shared bank reference. Other
 * decoders retain the queue. START resumes the actual saved position. RELEASE
 * discards queued output and frees scarce synthesis/transport resources while
 * retaining a prepared source and its heard position. SEEK uses microseconds,
 * discards old queued PCM, clamps to duration, and completes cooperatively.
 * Paused MIDI seeks retain the target without rendering; START
 * restores that position before succeeding.
 * GAIN is 0..100 and belongs to this player, separate from system volume.
 * Zero means accepted, -1 sets errno; accepted commands produce one RESULT.
 */
int fplinux_runtime_media_command(struct fplinux_runtime_media *media,
				  uint32_t handle,
				  enum fplinux_runtime_media_command command,
				  uint64_t value);
bool fplinux_runtime_media_info(struct fplinux_runtime_media *media,
				uint32_t handle,
				struct fplinux_runtime_media_info *info);
/* Guest foreground policy, including its runtime home screen. System focus
 * additionally holds the PCM stream. User pause never clears on focus return. */
void fplinux_runtime_media_set_visible(struct fplinux_runtime_media *media,
				       bool visible);

#endif
