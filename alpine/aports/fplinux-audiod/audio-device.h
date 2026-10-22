/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_DEVICE_H
#define FPLINUX_AUDIO_DEVICE_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#define FPLINUX_AUDIO_DEVICE_FDS 4U

struct _snd_pcm;

/*
 * The sound card, opened nonblocking for 48000 Hz signed 16-bit stereo with
 * an exact period and buffer, so that a busy card fails at once instead of
 * blocking the service. audiod never touches the mixer controls.
 */
struct fplinux_audio_device {
	const char *name;
	uint32_t period;
	uint32_t buffer;
	struct _snd_pcm *pcm;
	struct pollfd fds[FPLINUX_AUDIO_DEVICE_FDS];
	unsigned int fd_count;
	/* Output frames written or dropped since the service started. */
	uint64_t written;
};

/* Opens and prepares the card; 0 or a negative errno. */
int fplinux_audio_device_open(struct fplinux_audio_device *device);
/* Closes the card and drops the frames it still holds. */
void fplinux_audio_device_close(struct fplinux_audio_device *device);
/* Whether the card wants frames or has failed, given its poll results. */
bool fplinux_audio_device_ready(struct fplinux_audio_device *device,
				struct pollfd *fds);
/* Frames the card can take now, or a negative errno. */
long fplinux_audio_device_avail(struct fplinux_audio_device *device);
/* Writes interleaved frames; frames written or a negative errno. */
long fplinux_audio_device_write(struct fplinux_audio_device *device,
				const int16_t *frames, uint32_t count);
/* Prepares the card again after an underrun or a suspend. */
int fplinux_audio_device_recover(struct fplinux_audio_device *device);
/* Output frames that have left the card: written minus its delay. */
uint64_t fplinux_audio_device_played(struct fplinux_audio_device *device);
const char *fplinux_audio_device_error(int error);

#endif
