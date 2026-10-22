// SPDX-License-Identifier: GPL-2.0-only
/*
 * A fake playback card behind the ALSA calls audiod makes. It plays at
 * 48000 frames per second of the monotonic clock once its start threshold
 * is reached, wakes the poll loop each period through a timerfd, waits
 * instead of underrunning, and records what it does:
 *
 * - FPLINUX_AUDIOD_FAKE_EVENTS: a file of "<monotonic seconds> <event>"
 *   lines: open, busy, absent, hw <rate> <channels> <period> <buffer>,
 *   prepare and close;
 * - FPLINUX_AUDIOD_FAKE_PLAYED: every frame written, raw S16 stereo;
 * - FPLINUX_AUDIOD_FAKE_BUSY: while this file exists the card is busy;
 * - FPLINUX_AUDIOD_FAKE_ABSENT: when set, there is no card;
 * - FPLINUX_AUDIOD_FAKE_XRUN: the write with this number, counted from 1,
 *   fails with an underrun.
 */
#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000U

struct _snd_pcm {
	int timer;
	FILE *played;
	snd_pcm_hw_params_t hardware;
	snd_pcm_sw_params_t software;
	uint64_t written;
	bool started;
	uint64_t start_ns;
};

static struct _snd_pcm card = { .timer = -1 };
static bool card_open;
static unsigned long writes;

static uint64_t now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}

static void event(const char *format, ...)
{
	const char *path = getenv("FPLINUX_AUDIOD_FAKE_EVENTS");
	uint64_t now = now_ns();
	va_list arguments;
	FILE *file;

	if (!path || !(file = fopen(path, "a")))
		return;
	fprintf(file, "%llu.%06llu ", (unsigned long long)(now / 1000000000U),
		(unsigned long long)(now % 1000000000U / 1000U));
	va_start(arguments, format);
	vfprintf(file, format, arguments);
	va_end(arguments);
	fputc('\n', file);
	fclose(file);
}

/* Frames played; a card that ran dry waits for more instead of failing. */
static uint64_t consumed(snd_pcm_t *pcm)
{
	uint64_t now = now_ns(), frames;

	if (!pcm->started)
		return 0;
	frames = (now - pcm->start_ns) * RATE / 1000000000U;
	if (frames <= pcm->written)
		return frames;
	pcm->start_ns = now - pcm->written * 1000000000U / RATE;
	return pcm->written;
}

static snd_pcm_uframes_t delay_of(snd_pcm_t *pcm)
{
	return (snd_pcm_uframes_t)(pcm->written - consumed(pcm));
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream,
		 int mode)
{
	const char *busy = getenv("FPLINUX_AUDIOD_FAKE_BUSY");
	const char *played = getenv("FPLINUX_AUDIOD_FAKE_PLAYED");

	if (stream != SND_PCM_STREAM_PLAYBACK || mode != SND_PCM_NONBLOCK)
		return -EINVAL;
	if (getenv("FPLINUX_AUDIOD_FAKE_ABSENT")) {
		event("absent");
		return -ENOENT;
	}
	if (card_open || (busy && access(busy, F_OK) == 0)) {
		event("busy");
		return -EBUSY;
	}
	card = (struct _snd_pcm){ .timer = -1 };
	card.timer =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (card.timer < 0)
		return -errno;
	if (played)
		card.played = fopen(played, "ab");
	card_open = true;
	*pcm = &card;
	event("open %s", name);
	return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
	close(pcm->timer);
	if (pcm->played)
		fclose(pcm->played);
	card_open = false;
	event("close");
	return 0;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	(void)pcm;
	*params = (snd_pcm_hw_params_t){ 0 };
	return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				 snd_pcm_access_t access)
{
	(void)pcm;
	params->access = access;
	return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				 snd_pcm_format_t format)
{
	(void)pcm;
	params->format = format;
	return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				   unsigned int channels)
{
	(void)pcm;
	params->channels = channels;
	return 0;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
			       unsigned int rate, int direction)
{
	(void)pcm;
	if (direction)
		return -EINVAL;
	params->rate = rate;
	return 0;
}

int snd_pcm_hw_params_set_period_size(snd_pcm_t *pcm,
				      snd_pcm_hw_params_t *params,
				      snd_pcm_uframes_t frames, int direction)
{
	(void)pcm;
	if (direction)
		return -EINVAL;
	params->period = frames;
	return 0;
}

int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm,
				      snd_pcm_hw_params_t *params,
				      snd_pcm_uframes_t frames)
{
	(void)pcm;
	params->buffer = frames;
	return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
	pcm->written = 0;
	pcm->started = false;
	event("prepare");
	return 0;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	uint64_t period_ns;
	struct itimerspec tick;

	if (params->access != SND_PCM_ACCESS_RW_INTERLEAVED ||
	    params->format != SND_PCM_FORMAT_S16_LE || params->channels != 2 ||
	    params->rate != RATE || !params->period ||
	    params->buffer < 2 * params->period)
		return -EINVAL;
	pcm->hardware = *params;
	event("hw %u %u %lu %lu", params->rate, params->channels,
	      params->period, params->buffer);
	period_ns = (uint64_t)params->period * 1000000000U / RATE;
	tick.it_interval.tv_sec = (time_t)(period_ns / 1000000000U);
	tick.it_interval.tv_nsec = (long)(period_ns % 1000000000U);
	tick.it_value = tick.it_interval;
	if (timerfd_settime(pcm->timer, 0, &tick, NULL) < 0)
		return -errno;
	return snd_pcm_prepare(pcm);
}

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
	*params = (snd_pcm_sw_params_t){ .start_threshold = 1,
					 .avail_min = pcm->hardware.period };
	return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
					  snd_pcm_sw_params_t *params,
					  snd_pcm_uframes_t frames)
{
	(void)pcm;
	params->start_threshold = frames;
	return 0;
}

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
				    snd_pcm_uframes_t frames)
{
	(void)pcm;
	params->avail_min = frames;
	return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
	pcm->software = *params;
	return 0;
}

int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm)
{
	(void)pcm;
	return 1;
}

int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *fds,
			     unsigned int space)
{
	if (!space)
		return 0;
	fds[0] = (struct pollfd){ .fd = pcm->timer, .events = POLLIN };
	return 1;
}

snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
	return (snd_pcm_sframes_t)(pcm->hardware.buffer - delay_of(pcm));
}

int snd_pcm_poll_descriptors_revents(snd_pcm_t *pcm, struct pollfd *fds,
				     unsigned int count,
				     unsigned short *revents)
{
	uint64_t ticks;

	if (count != 1 || fds[0].fd != pcm->timer)
		return -EINVAL;
	if (read(pcm->timer, &ticks, sizeof(ticks)) < 0 && errno != EAGAIN)
		return -errno;
	*revents = snd_pcm_avail_update(pcm) >=
				   (snd_pcm_sframes_t)pcm->software.avail_min ?
			   POLLOUT :
			   0;
	return 0;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *frames,
				 snd_pcm_uframes_t count)
{
	const char *xrun = getenv("FPLINUX_AUDIOD_FAKE_XRUN");
	snd_pcm_sframes_t room = snd_pcm_avail_update(pcm);

	if (xrun && strtoul(xrun, NULL, 10) == ++writes) {
		event("xrun");
		return -EPIPE;
	}
	if (room <= 0)
		return -EAGAIN;
	if (count > (snd_pcm_uframes_t)room)
		count = (snd_pcm_uframes_t)room;
	if (pcm->played) {
		fwrite(frames, 4, count, pcm->played);
		fflush(pcm->played);
	}
	pcm->written += count;
	if (!pcm->started && pcm->written >= pcm->software.start_threshold) {
		pcm->started = true;
		pcm->start_ns = now_ns();
	}
	return (snd_pcm_sframes_t)count;
}

int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay)
{
	*delay = (snd_pcm_sframes_t)delay_of(pcm);
	return 0;
}

const char *snd_strerror(int error)
{
	return strerror(error < 0 ? -error : error);
}
