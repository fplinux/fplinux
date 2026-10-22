/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_MIXER_H
#define FPLINUX_AUDIO_MIXER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The mixing core of fplinux-audiod, without I/O. Every period, each stream
 * adds its next frames into a 32-bit mix: a linear resampler with a 32.32
 * fixed-point phase takes 8000...48000 Hz to the output's 48000 Hz, a mono
 * stream feeds both channels, and a Q15 gain ramps linearly across the
 * period. The output stage saturates to signed 16 bits. Streams play at
 * unity gain; the ramps remove the clicks of a pause and a resume.
 *
 * The mixer reads a ring only through its private copies of the capacity
 * mask, the channel count and the read position, and only the frames
 * between that read position and the write counter the caller loaded.
 */
#define FPLINUX_AUDIO_MIXER_RATE 48000U
#define FPLINUX_AUDIO_MIXER_CHANNELS 2U
#define FPLINUX_AUDIO_MIXER_UNITY 32768
/*
 * Read positions remembered per stream for the heard position, one per
 * period; the output buffer may hold at most this many periods less two.
 */
#define FPLINUX_AUDIO_MIXER_HISTORY 16U

/* A Q15 gain that moves from current to target across the next period. */
struct fplinux_audio_mixer_gain {
	int32_t current;
	int32_t target;
};

/* The stream's read position after the period that ended at output. */
struct fplinux_audio_mixer_mark {
	uint64_t output;
	uint32_t read;
};

struct fplinux_audio_mixer_stream {
	const int16_t *frames;
	uint32_t mask;
	uint32_t channels;
	/* Stream frames per output frame, 32.32. */
	uint64_t step;
	/* The first frame still needed, and the phase past it, 0.32. */
	uint32_t read;
	uint32_t phase;
	uint32_t heard;
	/* Set by the caller between periods; a paused stream takes no frames. */
	bool paused;
	struct fplinux_audio_mixer_gain gain;
	/* What the stream added at the end of the last period, per channel. */
	int32_t last[FPLINUX_AUDIO_MIXER_CHANNELS];
	struct fplinux_audio_mixer_mark marks[FPLINUX_AUDIO_MIXER_HISTORY];
	unsigned int newest;
	unsigned int count;
};

/*
 * Starts a playing stream at unity gain with read and heard 0, for a ring
 * of capacity frames, a power of two, at a rate and channel count in range.
 * output is the output frame counter at which its first period starts.
 */
void fplinux_audio_mixer_stream_init(struct fplinux_audio_mixer_stream *stream,
				     const int16_t *frames, uint32_t capacity,
				     uint32_t rate, uint32_t channels,
				     uint64_t output);

/*
 * Mixes the stream's next period of frames output frames into mix, the
 * interleaved stereo accumulator, and remembers the read position for the
 * output counter output_end at the end of the period. write is the ring's
 * write counter, loaded once for the period and ignored while paused.
 *
 * A stream that runs dry adds what it has. A stream paused since the last
 * period takes nothing and adds, for this one period, a ramp from its last
 * sample to zero; on resume its gain ramps up from zero across the first
 * period, from the frame where it stopped.
 *
 * Returns the output frames the stream filled from its ring, or -1 without
 * mixing when write is more than the capacity ahead of the read position,
 * which ends the stream.
 */
long fplinux_audio_mixer_mix(struct fplinux_audio_mixer_stream *stream,
			     uint32_t write, int32_t *mix, uint32_t frames,
			     uint64_t output_end);

/* Adds a linear ramp from last, per channel, to zero across frames. */
void fplinux_audio_mixer_ramp_down(int32_t *mix, uint32_t frames,
				   const int32_t *last);

/*
 * Returns the stream position heard once the output counter played has
 * left the output: frames written minus those the output still holds. It
 * interpolates between the remembered periods, never falls and never passes
 * the read position.
 */
uint32_t fplinux_audio_mixer_heard(struct fplinux_audio_mixer_stream *stream,
				   uint64_t played);

/* Saturates a period of the mix to signed 16-bit interleaved stereo. */
void fplinux_audio_mixer_output(const int32_t *mix, int16_t *output,
				uint32_t frames);

#endif
