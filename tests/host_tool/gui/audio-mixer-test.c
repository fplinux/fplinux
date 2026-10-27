/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host unit test of fplinux-audiod's mixing core: the 48000 Hz identity,
 * interpolation at 24000, 22050 and 8000 Hz, mono to stereo, gain ramps,
 * saturation, a hostile write counter, the pause tail and resume ramp with
 * every frame played once, and the heard position through a pause and an
 * underrun. Expected values come from exact arithmetic, not from the mixer.
 */
#include "audio-mixer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 480U
#define RING 16384U

static unsigned int failures;

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void checkf(bool condition, const char *message, long index, double got,
		   double want)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s at %ld: got %.3f, want %.3f\n",
			message, index, got, want);
		++failures;
	}
}

/* A pattern with large and signed steps between neighbouring frames. */
static int16_t pattern(uint32_t frame, uint32_t channel)
{
	return (int16_t)((int32_t)((frame * 7919U + channel * 104729U) %
				   40001U) -
			 20000);
}

static int16_t *ring_of(uint32_t capacity, uint32_t channels, uint32_t first,
			uint32_t count)
{
	int16_t *ring = calloc((size_t)capacity * channels, sizeof(*ring));
	uint32_t frame, channel;

	if (!ring) {
		perror("calloc");
		exit(1);
	}
	for (frame = first; frame < first + count; ++frame)
		for (channel = 0; channel < channels; ++channel)
			ring[(size_t)(frame & (capacity - 1)) * channels +
			     channel] = pattern(frame, channel);
	return ring;
}

/* Mixes one period of one stream into a fresh mix and saturates it. */
static long period(struct fplinux_audio_mixer_stream *stream, uint32_t write,
		   int16_t *output, uint64_t *clock)
{
	int32_t mix[PERIOD * 2] = { 0 };
	long made;

	*clock += PERIOD;
	made = fplinux_audio_mixer_mix(stream, write, mix, PERIOD, *clock);
	fplinux_audio_mixer_output(mix, output, PERIOD);
	return made;
}

static void test_identity_and_dry(void)
{
	int16_t *ring = ring_of(RING, 2, 0, 2000);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;
	uint32_t p, i;
	bool exact = true;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 48000, 2, clock);
	for (p = 0; p < 4; ++p) {
		check(period(&stream, 2000, output, &clock) == PERIOD,
		      "48000 Hz fills a whole period");
		for (i = 0; i < PERIOD * 2; ++i)
			exact &= output[i] ==
				 pattern(p * PERIOD + i / 2, i % 2);
	}
	check(exact, "48000 Hz stereo plays each frame unchanged");
	check(stream.read == 4 * PERIOD, "48000 Hz takes one frame per frame");
	/* 80 frames are left: they all play, the rest of the period is quiet. */
	check(period(&stream, 2000, output, &clock) == 80,
	      "a dry stream adds what it has");
	exact = true;
	for (i = 0; i < PERIOD * 2; ++i)
		exact &= output[i] ==
			 (i < 160 ? pattern(1920 + i / 2, i % 2) : 0);
	check(exact, "a dry stream plays its last frames, then silence");
	check(stream.read == 2000, "a dry stream takes every frame written");
	check(period(&stream, 2000, output, &clock) == 0 && !output[0],
	      "an empty stream adds nothing");
	free(ring);
}

static void test_mono_to_stereo(void)
{
	int16_t *ring = ring_of(RING, 1, 0, PERIOD);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;
	bool both = true;
	uint32_t i;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 48000, 1, clock);
	period(&stream, PERIOD, output, &clock);
	for (i = 0; i < PERIOD; ++i)
		both &= output[2 * i] == pattern(i, 0) &&
			output[2 * i + 1] == pattern(i, 0);
	check(both, "a mono frame plays on both channels");
	free(ring);
}

/*
 * Output frame k of a stream at rate samples it at k * rate / 48000 frames:
 * linear interpolation, within the 15-bit fraction and the final shift.
 */
static void test_interpolation(uint32_t rate, uint32_t channels)
{
	uint32_t periods = 6, needed = periods * PERIOD * rate / 48000 + 2;
	int16_t *ring = ring_of(RING, channels, 0, needed);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;
	uint32_t p, i, channel;
	char message[64];

	snprintf(message, sizeof(message), "%u Hz interpolates linearly", rate);
	fplinux_audio_mixer_stream_init(&stream, ring, RING, rate, channels,
					clock);
	for (p = 0; p < periods; ++p) {
		period(&stream, needed, output, &clock);
		for (i = 0; i < PERIOD; ++i) {
			uint64_t k = (uint64_t)p * PERIOD + i;
			uint32_t whole = (uint32_t)(k * rate / 48000);
			double t = (double)(k * rate % 48000) / 48000.0;

			for (channel = 0; channel < 2; ++channel) {
				uint32_t source = channels == 2 ? channel : 0;
				double first = pattern(whole, source);
				double step =
					pattern(whole + 1, source) - first;
				double want = first + step * t;
				double got = output[2 * i + channel];

				checkf(fabs(got - want) <=
					       1.0 + fabs(step) / 32768.0,
				       message, (long)k, got, want);
			}
		}
	}
	check(stream.read == (uint32_t)((uint64_t)periods * PERIOD * rate /
					48000) ||
		      stream.read == (uint32_t)((uint64_t)periods * PERIOD *
						rate / 48000) -
					     1,
	      "the read position follows the rate");
	free(ring);
}

/* At 24000 Hz every other frame is the exact midpoint, floored. */
static void test_half_rate_exact(void)
{
	int16_t *ring = ring_of(RING, 1, 0, PERIOD);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;
	bool exact = true;
	uint32_t i;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 24000, 1, clock);
	period(&stream, PERIOD, output, &clock);
	for (i = 0; i < PERIOD / 2; ++i) {
		int32_t first = pattern(i, 0), next = pattern(i + 1, 0);
		int32_t half = (next - first) >= 0 ? (next - first) / 2 :
						     -((first - next + 1) / 2);

		exact &= output[4 * i] == first &&
			 output[4 * i + 2] == first + half;
	}
	check(exact, "24000 Hz alternates frames and floored midpoints");
	check(stream.read == PERIOD / 2,
	      "24000 Hz takes half a frame per frame");
	free(ring);
}

/* A resampled stream that runs dry holds its last frame to its end. */
static void test_dry_resampled(void)
{
	int16_t *ring = ring_of(RING, 1, 0, 100);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 24000, 1, clock);
	check(period(&stream, 100, output, &clock) == 200,
	      "a dry 24000 Hz stream fills two output frames per frame");
	check(output[2 * 198] == pattern(99, 0) &&
		      output[2 * 199] == pattern(99, 0) && !output[2 * 200],
	      "the last frame plays for its whole length, then silence");
	check(stream.read == 100, "a dry resampled stream takes every frame");
	free(ring);
}

static void test_resume_gain_ramp(void)
{
	int16_t ring[RING];
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 0;
	bool ramp = true;
	uint32_t i;

	for (i = 0; i < RING; ++i)
		ring[i] = 16384;
	fplinux_audio_mixer_stream_init(&stream, ring, RING, 48000, 1, clock);
	/* Paused before it ever played: no tail, and the gain drops to 0. */
	stream.paused = true;
	check(period(&stream, RING, output, &clock) == 0 && !output[0] &&
		      !output[PERIOD * 2 - 1],
	      "a stream paused before it played is silent");
	stream.paused = false;
	period(&stream, RING, output, &clock);
	check(output[0] == 0, "a resumed stream starts from zero");
	for (i = 0; i < PERIOD; ++i) {
		double want = 16384.0 * i / PERIOD;

		ramp &= fabs(output[2 * i] - want) <= 1.0 &&
			output[2 * i] == output[2 * i + 1] &&
			(!i || output[2 * i] >= output[2 * i - 2]);
	}
	check(ramp, "the resume gain ramps linearly across one period");
	period(&stream, RING, output, &clock);
	check(output[0] == 16384 && output[PERIOD * 2 - 1] == 16384,
	      "the gain reaches unity after one period");
}

static void test_saturation(void)
{
	int16_t loud[RING], quiet[RING];
	struct fplinux_audio_mixer_stream streams[4];
	int32_t mix[PERIOD * 2] = { 0 };
	int16_t output[PERIOD * 2];
	uint32_t i;

	for (i = 0; i < RING; ++i) {
		loud[i] = 30000;
		quiet[i] = -30000;
	}
	/* Left: two loud streams; right: two quiet ones. */
	for (i = 0; i < 4; ++i)
		fplinux_audio_mixer_stream_init(&streams[i],
						i < 2 ? loud : quiet, RING / 2,
						48000, 2, 0);
	for (i = 0; i < RING; i += 2) {
		loud[i + 1] = 0;
		quiet[i] = 0;
	}
	for (i = 0; i < 4; ++i)
		fplinux_audio_mixer_mix(&streams[i], RING / 2, mix, PERIOD,
					PERIOD);
	check(mix[0] == 60000 && mix[1] == -60000,
	      "the mix accumulates in 32 bits");
	fplinux_audio_mixer_output(mix, output, PERIOD);
	check(output[0] == 32767 && output[1] == -32768 &&
		      output[2 * PERIOD - 2] == 32767 &&
		      output[2 * PERIOD - 1] == -32768,
	      "the output saturates to 16 bits");
}

static void test_hostile_write(void)
{
	enum { CAPACITY = 64, GUARD = 64 };
	int16_t memory[GUARD + CAPACITY + GUARD];
	struct fplinux_audio_mixer_stream stream;
	int32_t mix[PERIOD * 2] = { 0 };
	bool inside = true;
	uint32_t i;

	for (i = 0; i < GUARD + CAPACITY + GUARD; ++i)
		memory[i] = 31111;
	for (i = 0; i < CAPACITY; ++i)
		memory[GUARD + i] = (int16_t)(i + 1);
	fplinux_audio_mixer_stream_init(&stream, memory + GUARD, CAPACITY,
					48000, 1, 0);
	check(fplinux_audio_mixer_mix(&stream, CAPACITY + 1, mix, PERIOD,
				      PERIOD) == -1,
	      "a write more than the capacity ahead is refused");
	check(fplinux_audio_mixer_mix(&stream, (uint32_t)-1, mix, PERIOD,
				      PERIOD) == -1,
	      "a write behind the read position is refused");
	check(!mix[0] && !mix[1] && stream.read == 0,
	      "a refused write mixes nothing and keeps the position");
	/* Counters near the wrap: indexes use the private mask only. */
	stream.read = 0xfffffff0U;
	stream.mask = CAPACITY - 1;
	check(fplinux_audio_mixer_mix(&stream, 0x30U, mix, PERIOD,
				      2 * PERIOD) == CAPACITY,
	      "a full ring across the counter wrap plays");
	for (i = 0; i < PERIOD * 2; ++i)
		inside &= mix[i] != 31111;
	check(inside, "the mixer never reads outside the ring");
	check(stream.read == 0x30U,
	      "the read position wraps with the counters");
}

/* Every frame plays once: the pause tail and resume ramp read nothing. */
static void test_pause_resume_identity(void)
{
	int16_t *ring = ring_of(RING, 2, 0, 4 * PERIOD);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	int32_t last;
	uint64_t clock = 0;
	bool ok = true;
	uint32_t i;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 48000, 2, clock);
	period(&stream, 4 * PERIOD, output, &clock);
	last = output[2 * PERIOD - 2];
	check(last == pattern(PERIOD - 1, 0),
	      "the stream plays before pausing");

	stream.paused = true;
	check(period(&stream, 4 * PERIOD, output, &clock) == 0,
	      "a paused stream takes no frames");
	check(stream.read == PERIOD, "a paused stream keeps its position");
	for (i = 0; i < PERIOD; ++i) {
		double want = (double)last * (PERIOD - 1 - i) / PERIOD;

		ok &= fabs(output[2 * i] - want) <=
		      1.0 + fabs((double)last) / 32768.0;
	}
	check(ok, "the pause tail ramps from the last sample");
	check(output[2 * PERIOD - 2] == 0 && output[2 * PERIOD - 1] == 0,
	      "the pause tail ends at zero");
	period(&stream, 4 * PERIOD, output, &clock);
	ok = true;
	for (i = 0; i < PERIOD * 2; ++i)
		ok &= !output[i];
	check(ok, "a paused stream is silent after its tail");

	stream.paused = false;
	period(&stream, 4 * PERIOD, output, &clock);
	ok = output[0] == 0 && output[1] == 0;
	for (i = 0; i < PERIOD * 2; ++i) {
		double frame = pattern(PERIOD + i / 2, i % 2);
		double want = frame * (i / 2) / PERIOD;

		ok &= fabs(output[i] - want) <= 1.0 + fabs(frame) / 32768.0;
	}
	check(ok, "the resume ramp plays the frames where the stream stopped");
	period(&stream, 4 * PERIOD, output, &clock);
	ok = true;
	for (i = 0; i < PERIOD * 2; ++i)
		ok &= output[i] == pattern(2 * PERIOD + i / 2, i % 2);
	check(ok, "after the ramp the next frames play unchanged");
	check(stream.read == 3 * PERIOD, "no frame was skipped or repeated");
	free(ring);
}

/*
 * A resampled stream paused for two periods continues exactly where an
 * uninterrupted twin was: the phase is kept as well as the frame.
 */
static void test_pause_resume_resampled(void)
{
	int16_t *ring = ring_of(RING, 1, 0, 8 * PERIOD);
	struct fplinux_audio_mixer_stream twin, paused;
	int16_t straight[PERIOD * 2], resumed[PERIOD * 2];
	uint64_t clock_twin = 0, clock_paused = 0;
	uint32_t p;

	fplinux_audio_mixer_stream_init(&twin, ring, RING, 22050, 1, 0);
	fplinux_audio_mixer_stream_init(&paused, ring, RING, 22050, 1, 0);
	for (p = 0; p < 2; ++p) {
		period(&twin, 8 * PERIOD, straight, &clock_twin);
		period(&paused, 8 * PERIOD, resumed, &clock_paused);
	}
	paused.paused = true;
	period(&paused, 8 * PERIOD, resumed, &clock_paused);
	period(&paused, 8 * PERIOD, resumed, &clock_paused);
	paused.paused = false;
	period(&paused, 8 * PERIOD, resumed, &clock_paused);
	period(&twin, 8 * PERIOD, straight, &clock_twin);
	check(twin.read == paused.read && twin.phase == paused.phase,
	      "the resume period consumes what an unpaused period would");
	for (p = 0; p < 3; ++p) {
		period(&twin, 8 * PERIOD, straight, &clock_twin);
		period(&paused, 8 * PERIOD, resumed, &clock_paused);
		check(!memcmp(straight, resumed, sizeof(straight)),
		      "a resumed 22050 Hz stream continues exactly");
	}
	free(ring);
}

static void test_heard(void)
{
	int16_t *ring = ring_of(RING, 2, 0, RING);
	struct fplinux_audio_mixer_stream stream;
	int16_t output[PERIOD * 2];
	uint64_t clock = 1000;
	uint32_t previous, p;
	bool monotonic = true;

	fplinux_audio_mixer_stream_init(&stream, ring, RING, 48000, 2, clock);
	check(fplinux_audio_mixer_heard(&stream, 1000) == 0,
	      "a new stream has heard nothing");
	period(&stream, 10 * PERIOD, output, &clock); /* ends at 1480 */
	check(fplinux_audio_mixer_heard(&stream, 1000) == 0,
	      "nothing is heard before the output plays");
	check(fplinux_audio_mixer_heard(&stream, 1240) == 240,
	      "heard interpolates within a period");
	check(fplinux_audio_mixer_heard(&stream, 1480) == 480,
	      "heard reaches the period's read position");
	check(fplinux_audio_mixer_heard(&stream, 1300) == 480,
	      "heard never falls");
	period(&stream, 10 * PERIOD, output, &clock); /* 1960 */
	period(&stream, 10 * PERIOD, output, &clock); /* 2440 */
	check(fplinux_audio_mixer_heard(&stream, 2000) == 1000,
	      "heard follows the output across periods");
	check(fplinux_audio_mixer_heard(&stream, 1000000) == 1440 &&
		      stream.read == 1440,
	      "heard never passes the read position");

	/* Pause: the tail and a quiet period hold the read position. */
	stream.paused = true;
	period(&stream, 10 * PERIOD, output, &clock); /* 2920 */
	period(&stream, 10 * PERIOD, output, &clock); /* 3400 */
	check(fplinux_audio_mixer_heard(&stream, 3400) == 1440,
	      "a paused stream's heard catches up with its read position");
	stream.paused = false;
	period(&stream, 10 * PERIOD, output, &clock); /* 3880 */
	check(fplinux_audio_mixer_heard(&stream, 3640) == 1680,
	      "a resumed stream's heard moves again");

	/* Underrun: 100 frames left for a whole period. */
	period(&stream, 2020, output, &clock); /* 4360 */
	check(stream.read == 2020, "the dry stream took its last frames");
	check(fplinux_audio_mixer_heard(&stream, 4120) == 1970,
	      "a dry period's heard stays behind what played");
	check(fplinux_audio_mixer_heard(&stream, 4360) == 2020,
	      "a dry stream's heard catches up with its read position");
	period(&stream, 2020, output, &clock); /* 4840 */
	check(fplinux_audio_mixer_heard(&stream, 4840) == 2020,
	      "an empty stream's heard holds");

	/* Many periods with a delay of two and a half periods. */
	previous = stream.heard;
	for (p = 0; p < 40; ++p) {
		uint32_t heard;

		period(&stream, stream.read + PERIOD + (p % 3) * 100, output,
		       &clock);
		heard = fplinux_audio_mixer_heard(&stream,
						  clock - 5 * PERIOD / 2);
		monotonic &= (int32_t)(heard - previous) >= 0 &&
			     (int32_t)(stream.read - heard) >= 0;
		previous = heard;
	}
	check(monotonic,
	      "heard is monotonic and behind read over many periods");
	/* Older than the history: heard keeps its value. */
	check(fplinux_audio_mixer_heard(&stream, 1000) == previous,
	      "a position older than the history leaves heard unchanged");
	free(ring);
}

static void test_literal_resampling(void)
{
	const int16_t input[] = { 0, 1000, -1000, 2000 };
	const int16_t expected[] = {
		0,     0,     500, 500, 1000, 1000, 0,	  0,
		-1000, -1000, 500, 500, 2000, 2000, 2000, 2000,
	};
	struct fplinux_audio_mixer_stream stream;
	int32_t mix[16] = { 0 };
	int16_t output[16];

	fplinux_audio_mixer_stream_init(&stream, input, 4, 24000, 1, 0);
	check(fplinux_audio_mixer_mix(&stream, 4, mix, 8, 8) == 8,
	      "four 24000 Hz frames fill eight output frames");
	fplinux_audio_mixer_output(mix, output, 8);
	check(!memcmp(output, expected, sizeof(expected)),
	      "literal interpolation duplicates mono and holds the last sample");
	check(stream.read == 4 && fplinux_audio_mixer_heard(&stream, 8) == 4,
	      "literal resampling publishes all four input frames");
}

int main(void)
{
	test_literal_resampling();
	test_identity_and_dry();
	test_mono_to_stereo();
	test_interpolation(44100, 2);
	test_interpolation(24000, 1);
	test_interpolation(22050, 1);
	test_interpolation(22050, 2);
	test_interpolation(8000, 1);
	test_half_rate_exact();
	test_dry_resampled();
	test_resume_gain_ramp();
	test_saturation();
	test_hostile_write();
	test_pause_resume_identity();
	test_pause_resume_resampled();
	test_heard();
	if (failures) {
		fprintf(stderr, "%u failures\n", failures);
		return 1;
	}
	puts("audio mixer: ok");
	return 0;
}
