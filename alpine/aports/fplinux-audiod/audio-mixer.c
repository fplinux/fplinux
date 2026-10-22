// SPDX-License-Identifier: GPL-2.0-only
#include "audio-mixer.h"

#include <stddef.h>

#define UNITY FPLINUX_AUDIO_MIXER_UNITY
#define HISTORY FPLINUX_AUDIO_MIXER_HISTORY

void fplinux_audio_mixer_stream_init(struct fplinux_audio_mixer_stream *stream,
				     const int16_t *frames, uint32_t capacity,
				     uint32_t rate, uint32_t channels,
				     uint64_t output)
{
	*stream = (struct fplinux_audio_mixer_stream){
		.frames = frames,
		.mask = capacity - 1,
		.channels = channels,
		.step = ((uint64_t)rate << 32) / FPLINUX_AUDIO_MIXER_RATE,
		.gain = { .current = UNITY, .target = UNITY },
		.marks = { { .output = output } },
		.count = 1,
	};
}

/* A mono frame feeds both channels. */
static void frame_at(const struct fplinux_audio_mixer_stream *stream,
		     uint32_t index, int32_t *left, int32_t *right)
{
	size_t at = (size_t)(index & stream->mask) * stream->channels;

	*left = stream->frames[at];
	*right = stream->frames[at + stream->channels - 1];
}

/* Interpolates between two samples at a Q15 fraction, then scales. */
static int32_t sample(int32_t first, int32_t next, int32_t fraction,
		      int32_t gain)
{
	return ((first + (((next - first) * fraction) >> 15)) * gain) >> 15;
}

/*
 * The per-frame increment of a ramp held as a Q15 gain times 32768, which
 * reaches the target after frames; gains stay within 0...UNITY.
 */
static int32_t ramp_slope(const struct fplinux_audio_mixer_gain *gain,
			  uint32_t frames)
{
	return (gain->target - gain->current) * 32768 / (int32_t)frames;
}

static void remember(struct fplinux_audio_mixer_stream *stream, uint64_t output)
{
	stream->newest = (stream->newest + 1) % HISTORY;
	stream->marks[stream->newest] = (struct fplinux_audio_mixer_mark){
		.output = output,
		.read = stream->read,
	};
	if (stream->count < HISTORY)
		++stream->count;
}

/*
 * Output frame k samples the stream at read + offset, offset = phase +
 * k * step: frames whole and whole + 1, interpolated linearly. The last
 * frame written stands in for its missing successor, so a stream that runs
 * dry plays every frame it has.
 */
static uint32_t resample(struct fplinux_audio_mixer_stream *stream,
			 uint32_t available, int32_t *mix, uint32_t frames)
{
	uint64_t offset = stream->phase;
	int32_t level = stream->gain.current * 32768;
	int32_t slope = ramp_slope(&stream->gain, frames);
	int32_t left = 0, right = 0;
	uint32_t made;

	for (made = 0; made < frames; ++made) {
		uint32_t whole = (uint32_t)(offset >> 32);
		int32_t left0, right0, left1, right1, fraction, gain;

		if (whole >= available)
			break;
		frame_at(stream, stream->read + whole, &left0, &right0);
		if (whole + 1 < available) {
			frame_at(stream, stream->read + whole + 1, &left1,
				 &right1);
		} else {
			left1 = left0;
			right1 = right0;
		}
		fraction = (int32_t)((uint32_t)offset >> 17);
		gain = level >> 15;
		left = sample(left0, left1, fraction, gain);
		right = sample(right0, right1, fraction, gain);
		mix[2 * made] += left;
		mix[2 * made + 1] += right;
		level += slope;
		offset += stream->step;
	}
	/* At most available: the loop stops on the first frame missing. */
	stream->read += (uint32_t)(offset >> 32);
	stream->phase = (uint32_t)offset;
	stream->gain.current = stream->gain.target;
	stream->last[0] = made == frames ? left : 0;
	stream->last[1] = made == frames ? right : 0;
	return made;
}

long fplinux_audio_mixer_mix(struct fplinux_audio_mixer_stream *stream,
			     uint32_t write, int32_t *mix, uint32_t frames,
			     uint64_t output_end)
{
	uint32_t made = 0;

	if (stream->paused) {
		/* The tail and the resume ramp never read the ring. */
		fplinux_audio_mixer_ramp_down(mix, frames, stream->last);
		stream->last[0] = 0;
		stream->last[1] = 0;
		stream->gain.current = 0;
	} else {
		uint32_t available = write - stream->read;

		if (available > stream->mask + 1)
			return -1;
		made = resample(stream, available, mix, frames);
	}
	remember(stream, output_end);
	return made;
}

void fplinux_audio_mixer_ramp_down(int32_t *mix, uint32_t frames,
				   const int32_t *last)
{
	/* A Q15 factor times 32768 that reaches 0 at the last frame. */
	int32_t level = 1 << 30;
	int32_t slope = (int32_t)((1U << 30) / frames);
	uint32_t i;

	if (!last[0] && !last[1])
		return;
	for (i = 0; i < frames; ++i) {
		int64_t factor;

		level -= slope;
		factor = level >> 15;
		mix[2 * i] += (int32_t)((last[0] * factor) >> 15);
		mix[2 * i + 1] += (int32_t)((last[1] * factor) >> 15);
	}
}

uint32_t fplinux_audio_mixer_heard(struct fplinux_audio_mixer_stream *stream,
				   uint64_t played)
{
	unsigned int index = stream->newest, later = index, i;

	for (i = 0; i < stream->count; ++i) {
		const struct fplinux_audio_mixer_mark *mark =
			&stream->marks[index];

		if (mark->output <= played) {
			uint32_t heard = mark->read;

			if (i) {
				const struct fplinux_audio_mixer_mark *next =
					&stream->marks[later];

				heard += (uint32_t)((uint64_t)(next->read -
							       mark->read) *
						    (played - mark->output) /
						    (next->output -
						     mark->output));
			}
			if ((int32_t)(heard - stream->heard) > 0)
				stream->heard = heard;
			break;
		}
		later = index;
		index = (index + HISTORY - 1) % HISTORY;
	}
	return stream->heard;
}

static int16_t saturate(int64_t value)
{
	if (value > 32767)
		return 32767;
	if (value < -32768)
		return -32768;
	return (int16_t)value;
}

void fplinux_audio_mixer_output(const int32_t *mix, int16_t *output,
				uint32_t frames)
{
	uint32_t i;

	for (i = 0; i < 2 * frames; i += 2) {
		output[i] = saturate(mix[i]);
		output[i + 1] = saturate(mix[i + 1]);
	}
}
