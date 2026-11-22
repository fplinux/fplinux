// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "audio-device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* A deterministic paced sink; policy and ring handling stay in the daemon. */
static long available;
static int samples = -1;
static int events = -1;

static void event(const char *name)
{
	dprintf(events, "%s\n", name);
}

int fplinux_audio_device_init(struct fplinux_audio_device *device)
{
	samples = open(getenv("FPLINUX_TEST_PCM"), O_WRONLY | O_CREAT | O_TRUNC,
		       0600);
	events = open(getenv("FPLINUX_TEST_EVENTS"),
		      O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (mkfifo(getenv("FPLINUX_TEST_JACK"), 0600) < 0)
		return -errno;
	device->control_fd = open(getenv("FPLINUX_TEST_JACK"),
				  O_RDWR | O_NONBLOCK | O_CLOEXEC);
	device->radio_fd = -1;
	device->headphone_id = 1;
	device->speaker_id = getenv("FPLINUX_TEST_HEADPHONES_ONLY") ? 0 : 2;
	device->jack_id = 3;
	device->fm_id = 4;
	device->jack_present = device->speaker_id != 0;
	device->applied_route = FPLINUX_AUDIO_ROUTE_HEADPHONES;
	return samples < 0 || events < 0 || device->control_fd < 0 ? -errno : 0;
}

int fplinux_audio_device_route(struct fplinux_audio_device *device,
			       enum fplinux_audio_route route)
{
	device->route = route;
	device->applied_route =
		route == FPLINUX_AUDIO_ROUTE_AUTO ?
			(device->jack_present || !device->speaker_id ?
				 FPLINUX_AUDIO_ROUTE_HEADPHONES :
				 FPLINUX_AUDIO_ROUTE_SPEAKER) :
			route;
	event(device->applied_route == FPLINUX_AUDIO_ROUTE_HEADPHONES ?
		      "ROUTE HEADPHONES" :
		      "ROUTE SPEAKER");
	return 0;
}

int fplinux_audio_device_events(struct fplinux_audio_device *device)
{
	char change;

	if (read(device->control_fd, &change, 1) != 1)
		return -errno;
	device->jack_present = change == 'I';
	return FPLINUX_AUDIO_DEVICE_JACK_CHANGED |
	       (device->jack_present ? 0 : FPLINUX_AUDIO_DEVICE_PRIVATE_LOST);
}

int fplinux_audio_device_alarm(struct fplinux_audio_device *device, bool active)
{
	if (active && !device->jack_present && !device->speaker_id)
		return -ENODEV;
	device->alarm_active = active;
	return 0;
}

int fplinux_audio_device_tune(struct fplinux_audio_device *device,
			      unsigned int frequency)
{
	(void)device;
	(void)frequency;
	event("TUNE");
	return 0;
}

int fplinux_audio_device_fm(struct fplinux_audio_device *device, bool enabled,
			    unsigned int level)
{
	device->fm_muted = level == 0;
	device->fm_volume = level;
	if (enabled && device->pcm)
		return -EBUSY;
	if (enabled != device->fm_enabled)
		event(enabled ? "FM ON" : "FM OFF");
	device->fm_enabled = enabled;
	return 0;
}

bool fplinux_audio_device_fm_available(const struct fplinux_audio_device *device)
{
	return device->fm_id != 0;
}

int fplinux_audio_device_radio_close(struct fplinux_audio_device *device)
{
	return fplinux_audio_device_fm(device, false, 0);
}

int fplinux_audio_device_open(struct fplinux_audio_device *device)
{
	struct itimerspec tick = {
		.it_value = { .tv_nsec = 10000000 },
		.it_interval = { .tv_nsec = 10000000 },
	};
	int fd;

	if (device->fm_enabled)
		return -EBUSY;
	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (fd < 0 || timerfd_settime(fd, 0, &tick, NULL) < 0)
		return -errno;
	device->fds[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
	device->fd_count = 1;
	device->pcm = (struct _snd_pcm *)device;
	available = device->buffer;
	event("PCM OPEN");
	return 0;
}

void fplinux_audio_device_close(struct fplinux_audio_device *device)
{
	close(device->fds[0].fd);
	device->pcm = NULL;
	device->fd_count = 0;
	event("PCM CLOSE");
}

bool fplinux_audio_device_ready(struct fplinux_audio_device *device,
				struct pollfd *fds)
{
	uint64_t count;

	(void)fds;
	if (read(device->fds[0].fd, &count, sizeof(count)) > 0)
		available += (long)count * device->period;
	return true;
}

long fplinux_audio_device_avail(struct fplinux_audio_device *device)
{
	(void)device;
	return available;
}

long fplinux_audio_device_write(struct fplinux_audio_device *device,
				const int16_t *frames, uint32_t count)
{
	ssize_t bytes = write(samples, frames, (size_t)count * 4);

	if (bytes != (ssize_t)count * 4)
		return -EIO;
	device->written += count;
	available -= count;
	return count;
}

int fplinux_audio_device_recover(struct fplinux_audio_device *device)
{
	(void)device;
	return 0;
}

uint64_t fplinux_audio_device_played(struct fplinux_audio_device *device)
{
	return device->written;
}

const char *fplinux_audio_device_error(int error)
{
	(void)error;
	return "test device error";
}

void fplinux_audio_device_cleanup(struct fplinux_audio_device *device)
{
	if (device->pcm)
		fplinux_audio_device_close(device);
	close(samples);
	close(events);
	close(device->control_fd);
}
