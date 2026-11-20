/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fplinux/fplinux-media.h>
#include <fplinux/fplinux-runtime-media.h>
#include "fixtures/runtime-media/ui-boundary.h"

static struct fplinux_runtime_media_event events[128];
static size_t event_count;
static bool require_drained = true;

static void record_event(const struct fplinux_runtime_media_event *event,
			 void *context)
{
	(void)context;
	runtime_media_test_assert_owner();
	assert(event_count < 128);
	if (event->type == FPLINUX_RUNTIME_MEDIA_END && require_drained)
		runtime_media_test_assert_drained();
	events[event_count++] = *event;
}

static struct fplinux_runtime_media_event
wait_event(uint32_t handle, enum fplinux_runtime_media_event_type type,
	   enum fplinux_runtime_media_command command)
{
	for (unsigned int step = 0; step < 400; step++) {
		for (size_t i = 0; i < event_count; i++)
			if (events[i].handle == handle &&
			    events[i].type == type &&
			    (type != FPLINUX_RUNTIME_MEDIA_RESULT ||
			     events[i].command == command)) {
				struct fplinux_runtime_media_event event =
					events[i];

				memmove(&events[i], &events[i + 1],
					(event_count - i - 1) *
						sizeof(*events));
				event_count--;
				return event;
			}
		runtime_media_test_tick();
	}
	assert(!"media event deadline");
	return (struct fplinux_runtime_media_event){ 0 };
}

static void command(struct fplinux_runtime_media *broker, uint32_t handle,
		    enum fplinux_runtime_media_command action, uint64_t value)
{
	assert(fplinux_runtime_media_command(broker, handle, action, value) ==
	       0);
	assert(wait_event(handle, FPLINUX_RUNTIME_MEDIA_RESULT, action).error ==
	       0);
}

static const unsigned char wav[] = {
	'R',  'I',  'F', 'F',  52,   0,	   0, 0, 'W',  'A',  'V', 'E',
	'f',  'm',  't', ' ',  16,   0,	   0, 0, 1,    0,    1,	  0,
	0x40, 0x1f, 0,	 0,    0x80, 0x3e, 0, 0, 2,    0,    16,  0,
	'd',  'a',  't', 'a',  16,   0,	   0, 0, 0,    0x80, 0,	  0xc0,
	0,    0,    0,	 0x40, 0xff, 0x7f, 1, 0, 0xff, 0xff, 0,	  0x20,
};

static const unsigned char midi[] = {
	'M', 'T', 'h', 'd',  0,	  0,   0, 6,	0,    0,  0, 1,
	0,   96,  'M', 'T',  'r', 'k', 0, 0,	0,    12, 0, 0x90,
	69,  100, 96,  0x80, 69,  0,   0, 0xff, 0x2f, 0,
};

static struct {
	bool hold_open;
	struct fplinux_media *decoder;
	sem_t entered;
	sem_t proceed;
} canceled_prepare;

struct fplinux_media *__real_fplinux_media_open_bytes(const void *bytes,
						      size_t count);
void __real_fplinux_media_close(struct fplinux_media *media);

static void wait_decoder(sem_t *event)
{
	struct timespec deadline;
	int result;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 3;
	do {
		result = sem_timedwait(event, &deadline);
	} while (result < 0 && errno == EINTR);
	assert(result == 0);
}

static void hold_decoder(void)
{
	assert(sem_post(&canceled_prepare.entered) == 0);
	wait_decoder(&canceled_prepare.proceed);
}

struct fplinux_media *__wrap_fplinux_media_open_bytes(const void *bytes,
						      size_t count)
{
	if (!canceled_prepare.hold_open)
		return __real_fplinux_media_open_bytes(bytes, count);

	canceled_prepare.hold_open = false;
	hold_decoder();
	canceled_prepare.decoder =
		__real_fplinux_media_open_bytes(bytes, count);
	assert(canceled_prepare.decoder);
	return canceled_prepare.decoder;
}

void __wrap_fplinux_media_close(struct fplinux_media *media)
{
	if (media && media == canceled_prepare.decoder) {
		hold_decoder();
		canceled_prepare.decoder = NULL;
	}
	__real_fplinux_media_close(media);
}

static void test_destroy_during_prepare(struct fplinux_runtime_media *broker)
{
	struct fplinux_runtime_media_info info;
	uint32_t handle;

	assert(sem_init(&canceled_prepare.entered, 0, 0) == 0);
	assert(sem_init(&canceled_prepare.proceed, 0, 0) == 0);
	canceled_prepare.hold_open = true;
	handle = fplinux_runtime_media_create(broker, FPLINUX_RUNTIME_MEDIA_WAV,
					      wav, sizeof(wav), 0, 0);
	assert(handle);
	wait_decoder(&canceled_prepare.entered);
	fplinux_runtime_media_destroy(broker, handle);
	assert(!fplinux_runtime_media_info(broker, handle, &info));
	assert(sem_post(&canceled_prepare.proceed) == 0);

	/* Let the UI dispatch after PREPARE, before decoder cleanup returns. */
	wait_decoder(&canceled_prepare.entered);
	runtime_media_test_tick();
	assert(event_count == 0);
	assert(runtime_media_test_opened() == 0);
	assert(runtime_media_test_accepted() == 0);
	assert(sem_post(&canceled_prepare.proceed) == 0);
}

static void wait_samples(unsigned int stream, size_t frames)
{
	for (unsigned int step = 0; step < 400; step++) {
		if (runtime_media_test_stream_accepted(stream) >= frames)
			return;
		runtime_media_test_tick();
	}
	assert(!"media PCM deadline");
}

static bool has_signal(const int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (samples[i])
			return true;
	return false;
}

static void test_amr_nb_format(struct fplinux_runtime_media *broker)
{
	const unsigned char amr[] = {
		'#',  '!',  'A',  'M',	'R',  '\n', 0x3c, 0xf8, 0x03, 0xb7,
		0x48, 0x65, 0x9b, 0xc7, 0xcc, 0x88, 0x6c, 0xfa, 0xe4, 0x33,
		0x55, 0x74, 0xc0, 0x00, 0x00, 0xe0, 0x18, 0x30, 0x93, 0xa8,
		0x00, 0x00, 0x0d, 0x41, 0x10, 0x3e, 0xe2, 0x50,
	};
	const enum fplinux_runtime_media_format formats[] = {
		FPLINUX_RUNTIME_MEDIA_AUTO,
		FPLINUX_RUNTIME_MEDIA_AMR_NB,
		FPLINUX_RUNTIME_MEDIA_WAV,
	};

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		uint32_t handle = fplinux_runtime_media_create(
			broker, formats[i], amr, sizeof(amr), 0, 0);
		assert(handle);
		struct fplinux_runtime_media_event prepared =
			wait_event(handle, FPLINUX_RUNTIME_MEDIA_RESULT,
				   FPLINUX_RUNTIME_MEDIA_PREPARE);

		if (formats[i] == FPLINUX_RUNTIME_MEDIA_WAV) {
			assert(prepared.error == ENOTSUP);
		} else {
			assert(!prepared.error && prepared.info.rate == 8000 &&
			       prepared.info.channels == 1 &&
			       prepared.info.format ==
				       FPLINUX_RUNTIME_MEDIA_AMR_NB);
			command(broker, handle, FPLINUX_RUNTIME_MEDIA_START, 0);
			struct fplinux_runtime_media_event ended =
				wait_event(handle, FPLINUX_RUNTIME_MEDIA_END,
					   FPLINUX_RUNTIME_MEDIA_PREPARE);
			assert(ended.info.position_us == 20000 &&
			       ended.info.duration_us == 20000);
		}
		fplinux_runtime_media_destroy(broker, handle);
	}

	uint32_t wrong = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_AMR_NB, wav, sizeof(wav), 0, 0);
	assert(wrong);
	assert(wait_event(wrong, FPLINUX_RUNTIME_MEDIA_RESULT,
			  FPLINUX_RUNTIME_MEDIA_PREPARE)
		       .error == ENOTSUP);
	fplinux_runtime_media_destroy(broker, wrong);

	uint32_t soundtrack = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_MIDI, midi, sizeof(midi), 0, 0);
	uint32_t effects[16];

	assert(soundtrack);
	assert(!wait_event(soundtrack, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
		effects[i] = fplinux_runtime_media_create(
			broker, FPLINUX_RUNTIME_MEDIA_AMR_NB, amr, sizeof(amr),
			0, 0);
		assert(effects[i]);
		assert(!wait_event(effects[i], FPLINUX_RUNTIME_MEDIA_RESULT,
				   FPLINUX_RUNTIME_MEDIA_PREPARE)
				.error);
	}
	for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++)
		fplinux_runtime_media_destroy(broker, effects[i]);
	fplinux_runtime_media_destroy(broker, soundtrack);
}

static void test_mp3_playback(struct fplinux_runtime_media *broker,
			      const char *path,
			      enum fplinux_runtime_media_format requested,
			      enum fplinux_runtime_media_format expected)
{
	runtime_media_test_reset_samples();
	FILE *file = fopen(path, "rb");

	assert(file && fseek(file, 0, SEEK_END) == 0);
	long byte_count = ftell(file);

	assert(byte_count > 0 && byte_count < 1024 * 1024);
	assert(fseek(file, 0, SEEK_SET) == 0);
	void *mp3_bytes = malloc((size_t)byte_count);

	assert(mp3_bytes && fread(mp3_bytes, 1, (size_t)byte_count, file) ==
				    (size_t)byte_count);
	fclose(file);
	uint32_t mp3 = fplinux_runtime_media_create(
		broker, requested, mp3_bytes, (size_t)byte_count, 0, 0);

	free(mp3_bytes);
	assert(mp3);
	struct fplinux_runtime_media_event prepared =
		wait_event(mp3, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE);

	assert(!prepared.error && prepared.info.format == expected);
	struct fplinux_runtime_media_info info;
	command(broker, mp3, FPLINUX_RUNTIME_MEDIA_SEEK, 1000000);
	assert(fplinux_runtime_media_info(broker, mp3, &info));
	assert(info.position_us == 1000000 && info.duration_us == 2000000);
	assert(info.rate == 44100 && info.channels == 1);
	command(broker, mp3, FPLINUX_RUNTIME_MEDIA_START, 0);
	assert(wait_event(mp3, FPLINUX_RUNTIME_MEDIA_END,
			  FPLINUX_RUNTIME_MEDIA_PREPARE)
		       .info.position_us == 2000000);
	const int16_t mp3_reference[] = {
		6,     748,   1487,  2221,  2946,  3659,  4358,	 5039,
		5701,  6341,  6956,  7543,  8101,  8627,  9119,	 9575,
		9994,  10374, 10712, 11009, 11263, 11472, 11636, 11755,
		11827, 11853, 11832, 11765, 11652, 11492, 11288, 11040,
	};
	int16_t mp3_samples[32];

	assert(runtime_media_test_samples(mp3_samples, 32) == 32);
	assert(runtime_media_test_accepted() == 44100);
	for (size_t i = 0; i < 32; i++)
		assert(abs(mp3_samples[i] - mp3_reference[i]) <= 32);
	fplinux_runtime_media_destroy(broker, mp3);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	struct fplinux_ui *ui = runtime_media_test_ui_open();
	struct fplinux_runtime_media *broker =
		fplinux_runtime_media_open(ui, record_event, NULL);

	assert(broker);
	test_destroy_during_prepare(broker);
	uint32_t player = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_WAV, wav, sizeof(wav), 0, 0);

	assert(player);
	struct fplinux_runtime_media_event prepared =
		wait_event(player, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE);

	assert(!prepared.error && prepared.info.rate == 8000);
	assert(event_count == 0);
	assert(prepared.info.channels == 1 &&
	       prepared.info.duration_us == 1000);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_GAIN, 50);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_START, 0);
	for (unsigned int i = 0; runtime_media_test_accepted() < 8 && i < 100;
	     i++)
		runtime_media_test_tick();
	const int16_t expected[] = { -16384, -8192, 0, 8192, 16383, 0, 0, 4096 };

	int16_t heard[8];

	assert(runtime_media_test_samples(heard, 8) == 8);
	assert(runtime_media_test_accepted() == 8);
	assert(!memcmp(heard, expected, sizeof(expected)));
	assert(runtime_media_test_active_timers() == 1);
	fplinux_runtime_media_set_visible(broker, false);
	runtime_media_test_tick();
	assert(runtime_media_test_active_timers() == 0);
	assert(runtime_media_test_heard() == 0);
	fplinux_runtime_media_set_visible(broker, true);
	runtime_media_test_tick();
	assert(runtime_media_test_active_timers() == 1);
	for (unsigned int i = 0; i < 4; i++)
		runtime_media_test_tick();
	for (size_t i = 0; i < event_count; i++)
		assert(events[i].type != FPLINUX_RUNTIME_MEDIA_END);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	struct fplinux_runtime_media_info info;

	assert(fplinux_runtime_media_info(broker, player, &info));
	assert(info.position_us == 0 && !info.playing);
	runtime_media_test_set_drain(true);
	fplinux_runtime_media_set_visible(broker, false);
	for (unsigned int i = 0; i < 3; i++)
		runtime_media_test_tick();
	fplinux_runtime_media_set_visible(broker, true);
	assert(fplinux_runtime_media_info(broker, player, &info));
	assert(info.position_us == 0 && !info.playing);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_START, 0);
	struct fplinux_runtime_media_event ended =
		wait_event(player, FPLINUX_RUNTIME_MEDIA_END,
			   FPLINUX_RUNTIME_MEDIA_PREPARE);

	assert(ended.info.position_us == 1000 && !ended.info.playing);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_RELEASE, 0);
	assert(fplinux_runtime_media_info(broker, player, &info));
	assert(info.position_us == 1000 && !info.playing);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_PREFETCH, 0);
	assert(fplinux_runtime_media_command(
		       broker, player, FPLINUX_RUNTIME_MEDIA_START, 0) == 0);
	struct fplinux_runtime_media_event restarted =
		wait_event(player, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_START);

	assert(!restarted.error && restarted.info.position_us == 0);
	wait_event(player, FPLINUX_RUNTIME_MEDIA_END,
		   FPLINUX_RUNTIME_MEDIA_PREPARE);
	assert(runtime_media_test_accepted() == 16);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_SEEK, 500);
	assert(fplinux_runtime_media_info(broker, player, &info));
	assert(info.position_us == 0);
	command(broker, player, FPLINUX_RUNTIME_MEDIA_START, 0);
	wait_event(player, FPLINUX_RUNTIME_MEDIA_END,
		   FPLINUX_RUNTIME_MEDIA_PREPARE);
	fplinux_runtime_media_destroy(broker, player);

	unsigned int before_invalid = runtime_media_test_opened();
	uint32_t invalid = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_WAV, "invalid", 7, 0, 0);

	assert(invalid);
	assert(wait_event(invalid, FPLINUX_RUNTIME_MEDIA_RESULT,
			  FPLINUX_RUNTIME_MEDIA_PREPARE)
		       .error != 0);
	assert(runtime_media_test_opened() == before_invalid);
	assert(fplinux_runtime_media_command(
		       broker, invalid, FPLINUX_RUNTIME_MEDIA_START, 0) == -1);
	fplinux_runtime_media_destroy(broker, invalid);

	size_t before_held = runtime_media_test_accepted();
	uint32_t held = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_WAV, wav, sizeof(wav), 0, 0);

	assert(held);
	assert(!wait_event(held, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	runtime_media_test_set_policy_hold(true);
	command(broker, held, FPLINUX_RUNTIME_MEDIA_START, 0);
	for (unsigned int i = 0; i < 3; i++)
		runtime_media_test_tick();
	assert(runtime_media_test_accepted() == before_held);
	assert(fplinux_runtime_media_info(broker, held, &info));
	assert(info.playing && info.position_us == 0);
	fplinux_runtime_media_set_visible(broker, false);
	runtime_media_test_set_policy_hold(false);
	for (unsigned int i = 0; i < 3; i++)
		runtime_media_test_tick();
	assert(runtime_media_test_accepted() == before_held);
	fplinux_runtime_media_set_visible(broker, true);
	wait_event(held, FPLINUX_RUNTIME_MEDIA_END,
		   FPLINUX_RUNTIME_MEDIA_PREPARE);
	assert(runtime_media_test_accepted() == before_held + 8);
	fplinux_runtime_media_destroy(broker, held);

	uint32_t broken = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_WAV, wav, sizeof(wav), 0, 0);

	assert(broken);
	assert(!wait_event(broken, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	runtime_media_test_set_drain(false);
	size_t before_failure = runtime_media_test_accepted();

	command(broker, broken, FPLINUX_RUNTIME_MEDIA_START, 0);
	for (unsigned int i = 0;
	     runtime_media_test_accepted() == before_failure && i < 100; i++)
		runtime_media_test_tick();
	assert(runtime_media_test_accepted() > before_failure);
	runtime_media_test_set_output_failure(true);
	assert(wait_event(broken, FPLINUX_RUNTIME_MEDIA_ERROR,
			  FPLINUX_RUNTIME_MEDIA_PREPARE)
		       .error == EPIPE);
	assert(fplinux_runtime_media_info(broker, broken, &info));
	assert(!info.playing && info.position_us == 0);
	runtime_media_test_set_output_failure(false);
	runtime_media_test_set_drain(true);
	fplinux_runtime_media_destroy(broker, broken);

	require_drained = false;
	runtime_media_test_set_write_limit(511);
	test_mp3_playback(broker, argv[1], FPLINUX_RUNTIME_MEDIA_MP3,
			  FPLINUX_RUNTIME_MEDIA_MP3);
	test_mp3_playback(broker, argv[2], FPLINUX_RUNTIME_MEDIA_WAV,
			  FPLINUX_RUNTIME_MEDIA_WAV);
	test_mp3_playback(broker, argv[2], FPLINUX_RUNTIME_MEDIA_MP3,
			  FPLINUX_RUNTIME_MEDIA_MP3);
	test_mp3_playback(broker, argv[2], FPLINUX_RUNTIME_MEDIA_AUTO,
			  FPLINUX_RUNTIME_MEDIA_MP3);

	uint32_t first = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_MIDI, midi, sizeof(midi), 0, 0);
	uint32_t second = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_MIDI, midi, sizeof(midi), 0, 0);

	assert(first && second && first != second);
	assert(!wait_event(first, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	assert(!wait_event(second, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_PREFETCH, 0);
	command(broker, second, FPLINUX_RUNTIME_MEDIA_PREFETCH, 0);
	uint32_t additional[10];

	for (size_t i = 0; i < 10; i++) {
		additional[i] = fplinux_runtime_media_create(
			broker, FPLINUX_RUNTIME_MEDIA_MIDI, midi, sizeof(midi),
			0, 0);
		assert(additional[i]);
		assert(!wait_event(additional[i], FPLINUX_RUNTIME_MEDIA_RESULT,
				   FPLINUX_RUNTIME_MEDIA_PREPARE)
				.error);
		command(broker, additional[i], FPLINUX_RUNTIME_MEDIA_PREFETCH,
			0);
	}
	runtime_media_test_set_open_error(ECONNREFUSED);
	assert(fplinux_runtime_media_command(
		       broker, first, FPLINUX_RUNTIME_MEDIA_START, 0) == 0);
	assert(wait_event(first, FPLINUX_RUNTIME_MEDIA_RESULT,
			  FPLINUX_RUNTIME_MEDIA_START)
		       .error == ECONNREFUSED);
	assert(fplinux_runtime_media_info(broker, first, &info));
	assert(!info.playing);
	runtime_media_test_set_open_error(0);
	runtime_media_test_set_drain(false);
	command(broker, second, FPLINUX_RUNTIME_MEDIA_START, 0);
	command(broker, second, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_START, 0);
	unsigned int solo_stream = runtime_media_test_opened();
	int16_t solo[512], overlapping[512];

	wait_samples(solo_stream, 256);
	assert(runtime_media_test_stream_samples(solo_stream, solo, 512) ==
	       512);
	assert(has_signal(solo, 512));
	command(broker, first, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_SEEK, 0);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_START, 0);
	unsigned int first_stream = runtime_media_test_opened();

	command(broker, second, FPLINUX_RUNTIME_MEDIA_GAIN, 0);
	command(broker, second, FPLINUX_RUNTIME_MEDIA_START, 0);
	unsigned int second_stream = runtime_media_test_opened();

	wait_samples(first_stream, 256);
	wait_samples(second_stream, 256);
	assert(runtime_media_test_stream_samples(first_stream, overlapping,
						 512) == 512);
	assert(!memcmp(solo, overlapping, sizeof(solo)));
	assert(runtime_media_test_stream_samples(second_stream, overlapping,
						 512) == 512);
	assert(!has_signal(overlapping, 512));
	assert(runtime_media_test_stream_heard(first_stream) == 0);
	assert(runtime_media_test_stream_heard(second_stream) == 0);
	uint32_t tone = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_TONE, NULL, 0, 69, 20);

	assert(tone);
	assert(!wait_event(tone, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	command(broker, tone, FPLINUX_RUNTIME_MEDIA_START, 0);
	assert(fplinux_runtime_media_info(broker, tone, &info));
	assert(info.rate == 44100 && info.channels == 2);
	uint32_t overlapping_wav = fplinux_runtime_media_create(
		broker, FPLINUX_RUNTIME_MEDIA_WAV, wav, sizeof(wav), 0, 0);

	assert(overlapping_wav);
	assert(!wait_event(overlapping_wav, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_PREPARE)
			.error);
	command(broker, overlapping_wav, FPLINUX_RUNTIME_MEDIA_START, 0);
	assert(fplinux_runtime_media_command(broker, additional[0],
					     FPLINUX_RUNTIME_MEDIA_START,
					     0) == 0);
	assert(wait_event(additional[0], FPLINUX_RUNTIME_MEDIA_RESULT,
			  FPLINUX_RUNTIME_MEDIA_START)
		       .error == EMFILE);
	assert(fplinux_runtime_media_info(broker, first, &info) &&
	       info.playing && info.position_us == 0);
	assert(fplinux_runtime_media_info(broker, second, &info) &&
	       info.playing && info.position_us == 0);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	size_t frozen = runtime_media_test_stream_accepted(first_stream);

	command(broker, first, FPLINUX_RUNTIME_MEDIA_SEEK, 250000);
	assert(fplinux_runtime_media_info(broker, first, &info));
	assert(!info.playing && info.position_us == 249977);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_SEEK, 100000);
	assert(fplinux_runtime_media_info(broker, first, &info));
	assert(!info.playing && info.position_us == 100000);
	assert(fplinux_runtime_media_info(broker, second, &info) &&
	       info.playing);
	runtime_media_test_set_drain(true);
	for (unsigned int i = 0; i < 3; i++)
		runtime_media_test_tick();
	assert(runtime_media_test_stream_accepted(first_stream) == frozen);
	assert(runtime_media_test_stream_heard(second_stream) > 0);
	assert(fplinux_runtime_media_info(broker, first, &info));
	assert(!info.playing && info.position_us == 100000);
	command(broker, second, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	assert(fplinux_runtime_media_command(
		       broker, first, FPLINUX_RUNTIME_MEDIA_START, 0) == 0);
	struct fplinux_runtime_media_event restored =
		wait_event(first, FPLINUX_RUNTIME_MEDIA_RESULT,
			   FPLINUX_RUNTIME_MEDIA_START);

	assert(!restored.error && restored.info.playing);
	assert(restored.info.position_us == 100000);
	command(broker, first, FPLINUX_RUNTIME_MEDIA_RELEASE, 0);
	fplinux_runtime_media_destroy(broker, first);
	fplinux_runtime_media_destroy(broker, second);
	fplinux_runtime_media_destroy(broker, tone);
	fplinux_runtime_media_destroy(broker, overlapping_wav);
	for (size_t i = 0; i < 10; i++)
		fplinux_runtime_media_destroy(broker, additional[i]);

	test_amr_nb_format(broker);
	fplinux_runtime_media_close(broker);
	assert(sem_destroy(&canceled_prepare.proceed) == 0);
	assert(sem_destroy(&canceled_prepare.entered) == 0);
	runtime_media_test_ui_close();
	puts("runtime media: ok");
	return 0;
}
