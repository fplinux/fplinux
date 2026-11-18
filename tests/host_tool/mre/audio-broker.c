/* SPDX-License-Identifier: GPL-2.0-only */
#include "audio-broker.h"
#include "ui-boundary.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct reply_record {
	uint32_t sequence;
	int32_t result;
};

struct event_record {
	uint32_t handle;
	int32_t event;
};

static struct {
	struct mre_audio_broker *broker;
	uint32_t sequence;
	struct reply_record replies[128];
	struct event_record events[128];
	size_t reply_count, event_count;
} test;

/* C4 for one quarter beat at 96 PPQN: 125 ms at the standard default tempo. */
static const unsigned char midi[] = {
	'M',  'T', 'h', 'd', 0,	   0,  0, 6, 0,	   0,	 0,    1, 0,
	96,   'M', 'T', 'r', 'k',  0,  0, 0, 15,   0,	 0xc0, 0, 0,
	0x90, 60,  127, 24,  0x80, 60, 0, 0, 0xff, 0x2f, 0,
};

static bool on_reply(uint32_t sequence, int32_t result, void *context)
{
	(void)context;
	runtime_media_test_assert_owner();
	assert(test.reply_count < 128);
	test.replies[test.reply_count++] =
		(struct reply_record){ sequence, result };
	return true;
}

static bool on_event(uint32_t handle, int32_t event, void *context)
{
	(void)context;
	runtime_media_test_assert_owner();
	assert(test.event_count < 128);
	if (event == 2 || event == 5)
		runtime_media_test_assert_drained();
	test.events[test.event_count++] =
		(struct event_record){ handle, event };
	return true;
}

static void open_broker(void)
{
	memset(&test, 0, sizeof(test));
	test.broker = mre_audio_broker_open(runtime_media_test_ui_open(),
					    on_reply, on_event, NULL);
	assert(test.broker);
	runtime_media_test_set_write_limit(511);
}

static void close_broker(void)
{
	mre_audio_broker_close(test.broker);
	test.broker = NULL;
	runtime_media_test_ui_close();
}

static uint32_t submit(struct mre_audio_request *request)
{
	request->sequence = ++test.sequence;
	assert(mre_audio_broker_dispatch(test.broker, request));
	return request->sequence;
}

static int32_t wait_reply(uint32_t sequence)
{
	for (unsigned int tick = 0; tick < 1000; ++tick) {
		for (size_t i = 0; i < test.reply_count; ++i)
			if (test.replies[i].sequence == sequence)
				return test.replies[i].result;
		runtime_media_test_tick();
	}
	assert(!"MRE audio reply deadline");
	return -1;
}

static bool has_event(uint32_t handle, int32_t event)
{
	for (size_t i = 0; i < test.event_count; ++i)
		if (test.events[i].handle == handle &&
		    test.events[i].event == event)
			return true;
	return false;
}

static void wait_event(uint32_t handle, int32_t event)
{
	for (unsigned int tick = 0; tick < 1000; ++tick) {
		if (has_event(handle, event))
			return;
		runtime_media_test_tick();
	}
	assert(!"MRE audio event deadline");
}

static int32_t operation(uint32_t action, uint32_t handle)
{
	struct mre_audio_request request = { .operation = action,
					     .handle = handle };

	return wait_reply(submit(&request));
}

static int32_t play(uint32_t handle, uint32_t kind, uint32_t format,
		    const void *bytes, size_t count, int32_t repeats)
{
	struct mre_audio_request request = {
		.operation = MRE_AUDIO_BEGIN,
		.handle = handle,
		.kind = kind,
		.format = format,
		.total_bytes = (uint32_t)count,
		.repeat = repeats,
	};
	int32_t begun = wait_reply(submit(&request));

	if (begun < 0)
		return begun;
	for (size_t offset = 0; offset < count;) {
		size_t chunk = count - offset;

		if (chunk > 4096)
			chunk = 4096;
		memset(&request, 0, sizeof(request));
		request.operation = MRE_AUDIO_WRITE;
		request.handle = handle;
		request.offset = (uint32_t)offset;
		request.bytes = (uint32_t)chunk;
		memcpy(request.data, (const unsigned char *)bytes + offset,
		       chunk);
		assert(wait_reply(submit(&request)) == 0);
		offset += chunk;
	}
	return operation(MRE_AUDIO_PLAY, handle);
}

static void set_volume(int32_t level)
{
	struct mre_audio_request request = {
		.operation = MRE_AUDIO_VOLUME,
		.volume = level,
	};

	assert(wait_reply(submit(&request)) == 0);
}

static void wait_samples(size_t minimum)
{
	for (unsigned int tick = 0; tick < 500; ++tick) {
		if (runtime_media_test_accepted() >= minimum)
			return;
		runtime_media_test_tick();
	}
	assert(!"MRE audio PCM deadline");
}

static void
check_midi_loop_pause_and_heard_completion(uint64_t single_pass_frames)
{
	open_broker();
	set_volume(0);
	assert(play(1, 1, 17, midi, sizeof(midi), 2) == 1);
	wait_event(1, 1);
	wait_samples(32);
	int16_t samples[32];

	assert(runtime_media_test_samples(samples, 32) == 32);
	for (size_t i = 0; i < 32; ++i)
		assert(samples[i] == 0);
	assert(operation(MRE_AUDIO_PAUSE, 1) == 0);
	size_t paused = runtime_media_test_accepted();

	mre_audio_broker_set_visible(test.broker, false);
	for (unsigned int i = 0; i < 3; ++i)
		runtime_media_test_tick();
	mre_audio_broker_set_visible(test.broker, true);
	for (unsigned int i = 0; i < 3; ++i)
		runtime_media_test_tick();
	assert(runtime_media_test_accepted() == paused);
	assert(runtime_media_test_heard() == 0);
	assert(!has_event(1, 2));
	assert(operation(MRE_AUDIO_RESUME, 1) == 0);
	/* The resumed sequence queues real PCM while the output remains held. */
	wait_samples(paused + 5512);
	assert(runtime_media_test_heard() == 0);
	assert(!has_event(1, 2));
	runtime_media_test_set_drain(true);
	wait_event(1, 2);
	assert(test.event_count == 2);
	assert(runtime_media_test_heard() == single_pass_frames * 2);
	assert(operation(MRE_AUDIO_RESUME, 1) == -1);
	close_broker();
}

static uint64_t finite_midi_frames(int32_t repeat)
{
	open_broker();
	runtime_media_test_set_drain(true);
	assert(play(1, 1, 17, midi, sizeof(midi), repeat) == 1);
	wait_event(1, 1);
	wait_event(1, 2);
	assert(test.event_count == 2);
	uint64_t frames = runtime_media_test_heard();

	close_broker();
	return frames;
}

static uint64_t check_finite_repeat_renders_the_complete_sequence_twice(void)
{
	uint64_t once = finite_midi_frames(1);
	uint64_t twice = finite_midi_frames(2);

	/* Repeat counts whole passes, regardless of transport reopen strategy. */
	assert(once >= 5512);
	assert(twice == once * 2);
	return once;
}

static void
check_zero_repeat_and_stop_cancel_events(uint64_t single_pass_frames)
{
	open_broker();
	runtime_media_test_set_drain(true);
	assert(play(1, 1, 17, midi, sizeof(midi), 0) == 1);
	wait_event(1, 1);
	for (unsigned int tick = 0;
	     runtime_media_test_heard() <= single_pass_frames && tick < 1000;
	     ++tick)
		runtime_media_test_tick();
	assert(runtime_media_test_heard() > single_pass_frames);
	assert(!has_event(1, 2));
	assert(operation(MRE_AUDIO_STOP, 1) == 0);
	size_t stopped = runtime_media_test_accepted();

	for (unsigned int i = 0; i < 4; ++i)
		runtime_media_test_tick();
	assert(runtime_media_test_accepted() == stopped && !has_event(1, 2));
	assert(play(2, 1, 17, midi, sizeof(midi), 255) == 2);
	assert(operation(MRE_AUDIO_STOP_MIDI, 0) == 0);
	close_broker();
}

static void check_decode_failure_and_replacement_cleanup(void)
{
	open_broker();
	const unsigned char invalid[] = { 'M', 'T', 'h', 'd', 0 };

	assert(play(1, 1, 17, invalid, sizeof(invalid), 1) == -1);
	assert(test.event_count == 0 && runtime_media_test_opened() == 0);
	assert(play(2, 1, 17, midi, sizeof(midi), 1) == 2);
	wait_event(2, 1);
	wait_samples(32);
	/* Close the actual decoder worker with queued output before new ownership. */
	close_broker();
	open_broker();
	for (unsigned int i = 0; i < 4; ++i)
		runtime_media_test_tick();
	assert(test.reply_count == 0 && test.event_count == 0);
	assert(runtime_media_test_accepted() == 0);
	close_broker();
}

static void
check_mp3_default_gain_explicit_volume_and_callback(const char *path)
{
	FILE *file = fopen(path, "rb");

	assert(file && fseek(file, 0, SEEK_END) == 0);
	long size = ftell(file);

	assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
	unsigned char *bytes = malloc((size_t)size);

	assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
	assert(fclose(file) == 0);
	open_broker();
	assert(play(1, 2, 5, bytes, (size_t)size, 1) == 0);
	wait_samples(32);
	int16_t default_samples[32];
	bool nonzero = false;

	assert(runtime_media_test_samples(default_samples, 32) == 32);
	for (size_t i = 0; i < 32; ++i)
		nonzero |= default_samples[i] != 0;
	assert(nonzero);
	close_broker();

	open_broker();
	set_volume(6);
	runtime_media_test_set_drain(true);
	assert(play(1, 2, 13, bytes, (size_t)size, 1) == -1);
	assert(play(2, 2, 17, bytes, (size_t)size, 1) == -1);
	assert(play(3, 2, 5, bytes, (size_t)size, 1) == 0);
	wait_event(3, 5);
	assert(!has_event(3, 1) && test.event_count == 1);
	/* Independent two-second 44.1 kHz fixture, with gapless trim. */
	assert(runtime_media_test_accepted() == 88200);
	int16_t samples[32];

	assert(runtime_media_test_samples(samples, 32) == 32);
	assert(!memcmp(samples, default_samples, sizeof(default_samples)));
	close_broker();

	open_broker();
	set_volume(3);
	assert(play(1, 2, 5, bytes, (size_t)size, 1) == 0);
	wait_samples(32);
	assert(runtime_media_test_samples(samples, 32) == 32);
	for (size_t i = 0; i < 32; ++i)
		assert(samples[i] == default_samples[i] / 2);
	close_broker();
	free(bytes);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	uint64_t single_pass_frames =
		check_finite_repeat_renders_the_complete_sequence_twice();

	check_midi_loop_pause_and_heard_completion(single_pass_frames);
	check_zero_repeat_and_stop_cancel_events(single_pass_frames);
	check_decode_failure_and_replacement_cleanup();
	check_mp3_default_gain_explicit_volume_and_callback(argv[1]);
	puts("MRE audio broker: ok");
	return 0;
}
