/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "ui-boundary.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-audio.h>

struct watched_fd {
	int fd;
	short events;
	fplinux_ui_fd_cb callback;
	void *context;
};

struct fplinux_ui {
	pthread_t owner;
	struct watched_fd watches[8];
	size_t count;
};

struct fplinux_audio_stream {
	unsigned int id;
	unsigned int rate;
	unsigned int channels;
	size_t accepted;
	uint64_t heard;
	int16_t samples[512];
	long queued;
	bool paused;
	bool closed;
	struct fplinux_audio_stream *next;
};

static struct {
	struct fplinux_ui ui;
	struct fplinux_audio_stream *streams;
	unsigned int opened;
	unsigned int closed;
	size_t accepted;
	uint64_t heard;
	size_t write_limit;
	int16_t samples[32];
	bool drain;
	bool held;
	bool failed;
	int open_error;
} boundary;

void runtime_media_test_assert_owner(void)
{
	assert(pthread_equal(pthread_self(), boundary.ui.owner));
}

struct fplinux_ui *runtime_media_test_ui_open(void)
{
	memset(&boundary, 0, sizeof(boundary));
	boundary.ui.owner = pthread_self();
	boundary.write_limit = 3;
	return &boundary.ui;
}

void runtime_media_test_ui_close(void)
{
	runtime_media_test_assert_owner();
	assert(!boundary.ui.count && boundary.opened == boundary.closed);
	while (boundary.streams) {
		struct fplinux_audio_stream *next = boundary.streams->next;

		free(boundary.streams);
		boundary.streams = next;
	}
}

bool fplinux_ui_audio_supported(const struct fplinux_ui *ui)
{
	runtime_media_test_assert_owner();
	assert(ui == &boundary.ui);
	return true;
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	runtime_media_test_assert_owner();
	assert(ui == &boundary.ui);
	return true;
}

bool fplinux_ui_watch_fd(struct fplinux_ui *ui, int fd, short watched,
			 fplinux_ui_fd_cb callback, void *context)
{
	runtime_media_test_assert_owner();
	assert(ui == &boundary.ui && ui->count < 8);
	ui->watches[ui->count++] = (struct watched_fd){
		.fd = fd,
		.events = watched,
		.callback = callback,
		.context = context,
	};
	return true;
}

void fplinux_ui_unwatch_fd(struct fplinux_ui *ui, int fd)
{
	runtime_media_test_assert_owner();
	assert(ui == &boundary.ui);
	for (size_t i = 0; i < ui->count; i++)
		if (ui->watches[i].fd == fd) {
			ui->watches[i] = ui->watches[--ui->count];
			return;
		}
}

struct fplinux_audio_stream *fplinux_ui_audio_open(struct fplinux_ui *ui,
						   unsigned int rate,
						   unsigned int channels)
{
	runtime_media_test_assert_owner();
	assert(ui == &boundary.ui && rate >= 8000 && rate <= 48000);
	assert(channels == 1 || channels == 2);
	if (boundary.open_error) {
		errno = boundary.open_error;
		return NULL;
	}
	if (boundary.opened - boundary.closed == 4) {
		errno = EMFILE;
		return NULL;
	}
	struct fplinux_audio_stream *stream = calloc(1, sizeof(*stream));

	assert(stream);
	stream->rate = rate;
	stream->channels = channels;
	stream->id = boundary.opened + 1;
	stream->next = boundary.streams;
	boundary.streams = stream;
	boundary.opened++;
	return stream;
}

long fplinux_ui_audio_write(struct fplinux_audio_stream *stream,
			    const int16_t *frames, size_t count)
{
	runtime_media_test_assert_owner();
	assert(!stream->closed);
	if (boundary.failed) {
		errno = EPIPE;
		return -1;
	}
	if (count > boundary.write_limit)
		count = boundary.write_limit;
	size_t saved = stream->accepted * stream->channels;
	size_t available = sizeof(stream->samples) / sizeof(*stream->samples);

	if (saved < available) {
		size_t copied = count * stream->channels;

		if (copied > available - saved)
			copied = available - saved;
		memcpy(stream->samples + saved, frames,
		       copied * sizeof(*frames));
	}
	stream->accepted += count;
	for (size_t i = 0; i < count; i++) {
		if (boundary.accepted < 32)
			boundary.samples[boundary.accepted] =
				frames[i * stream->channels];
		boundary.accepted++;
	}
	stream->queued += (long)count;
	return (long)count;
}

long fplinux_ui_audio_queued(const struct fplinux_audio_stream *stream)
{
	runtime_media_test_assert_owner();
	assert(!stream->closed);
	return stream->queued;
}

bool fplinux_ui_audio_pause(struct fplinux_audio_stream *stream, bool paused)
{
	runtime_media_test_assert_owner();
	assert(!stream->closed);
	stream->paused = paused;
	return true;
}

bool fplinux_ui_audio_status(const struct fplinux_audio_stream *stream,
			     struct fplinux_ui_audio_status *status)
{
	runtime_media_test_assert_owner();
	assert(!stream->closed);
	status->state = stream->paused || boundary.held ?
				FPLINUX_UI_AUDIO_PAUSED :
				FPLINUX_UI_AUDIO_PLAYING;
	status->reason = boundary.held ? FPLINUX_UI_AUDIO_REASON_POLICY :
					 FPLINUX_UI_AUDIO_REASON_NONE;
	return true;
}

void fplinux_ui_audio_close(struct fplinux_audio_stream *stream)
{
	runtime_media_test_assert_owner();
	if (!stream)
		return;
	assert(!stream->closed);
	stream->closed = true;
	boundary.closed++;
}

void runtime_media_test_tick(void)
{
	struct fplinux_ui *ui = &boundary.ui;
	struct pollfd descriptors[8];
	struct watched_fd watched[8];
	size_t count = ui->count;

	runtime_media_test_assert_owner();
	memcpy(watched, ui->watches, count * sizeof(*watched));
	for (size_t i = 0; i < count; i++)
		descriptors[i] = (struct pollfd){
			.fd = watched[i].fd,
			.events = watched[i].events,
		};
	assert(poll(descriptors, count, 25) >= 0);
	for (size_t i = 0; i < count; i++)
		if (descriptors[i].revents)
			watched[i].callback(ui, descriptors[i].fd,
					    descriptors[i].revents,
					    watched[i].context);
	if (boundary.drain && !boundary.held)
		for (struct fplinux_audio_stream *stream = boundary.streams;
		     stream; stream = stream->next)
			if (!stream->closed && !stream->paused) {
				boundary.heard += (uint64_t)stream->queued;
				stream->heard += (uint64_t)stream->queued;
				stream->queued = 0;
			}
}

unsigned int runtime_media_test_active_timers(void)
{
	unsigned int active = 0;

	runtime_media_test_assert_owner();
	for (size_t i = 0; i < boundary.ui.count; i++) {
		struct itimerspec timer;

		if (timerfd_gettime(boundary.ui.watches[i].fd, &timer) == 0 &&
		    (timer.it_value.tv_sec || timer.it_value.tv_nsec))
			active++;
	}
	return active;
}

void runtime_media_test_set_drain(bool drain)
{
	boundary.drain = drain;
}

void runtime_media_test_set_policy_hold(bool held)
{
	boundary.held = held;
}

void runtime_media_test_set_output_failure(bool failed)
{
	boundary.failed = failed;
}

void runtime_media_test_set_open_error(int error)
{
	boundary.open_error = error;
}

void runtime_media_test_set_write_limit(size_t frames)
{
	boundary.write_limit = frames;
}

size_t runtime_media_test_accepted(void)
{
	return boundary.accepted;
}

uint64_t runtime_media_test_heard(void)
{
	return boundary.heard;
}

void runtime_media_test_reset_samples(void)
{
	runtime_media_test_assert_owner();
	boundary.accepted = 0;
	memset(boundary.samples, 0, sizeof(boundary.samples));
}

size_t runtime_media_test_samples(int16_t *samples, size_t capacity)
{
	size_t count = boundary.accepted < 32 ? boundary.accepted : 32;

	if (count > capacity)
		count = capacity;
	memcpy(samples, boundary.samples, count * sizeof(*samples));
	return count;
}

unsigned int runtime_media_test_opened(void)
{
	return boundary.opened;
}

unsigned int runtime_media_test_closed(void)
{
	return boundary.closed;
}

static struct fplinux_audio_stream *find_stream(unsigned int id)
{
	runtime_media_test_assert_owner();
	for (struct fplinux_audio_stream *stream = boundary.streams; stream;
	     stream = stream->next)
		if (stream->id == id)
			return stream;
	assert(!"unknown test transport");
	return NULL;
}

size_t runtime_media_test_stream_samples(unsigned int id, int16_t *samples,
					 size_t capacity)
{
	struct fplinux_audio_stream *stream = find_stream(id);
	size_t count = stream->accepted * stream->channels;
	size_t available = sizeof(stream->samples) / sizeof(*stream->samples);

	if (count > available)
		count = available;
	if (count > capacity)
		count = capacity;
	memcpy(samples, stream->samples, count * sizeof(*samples));
	return count;
}

size_t runtime_media_test_stream_accepted(unsigned int id)
{
	return find_stream(id)->accepted;
}

uint64_t runtime_media_test_stream_heard(unsigned int id)
{
	return find_stream(id)->heard;
}

void runtime_media_test_assert_drained(void)
{
	for (struct fplinux_audio_stream *stream = boundary.streams; stream;
	     stream = stream->next)
		assert(stream->closed || stream->queued == 0);
}
