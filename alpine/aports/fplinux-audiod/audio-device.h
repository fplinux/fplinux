/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_DEVICE_H
#define FPLINUX_AUDIO_DEVICE_H

#include "fplinux-audio-protocol.h"

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#define FPLINUX_AUDIO_DEVICE_FDS 4U

struct _snd_pcm;
struct _snd_ctl;

enum fplinux_audio_device_event {
	FPLINUX_AUDIO_DEVICE_JACK_CHANGED = 1,
	FPLINUX_AUDIO_DEVICE_PRIVATE_LOST = 2,
};

struct fplinux_audio_volume_control {
	unsigned int id;
	unsigned int count;
	long maximum;
	long saved[2];
};

/*
 * The sound card, opened nonblocking for native-rate signed 16-bit stereo with
 * an exact period and buffer, so that a busy card fails at once instead of
 * blocking the service. The same owner controls routes, jack and FM.
 */
struct fplinux_audio_device {
	const char *name;
	uint32_t period;
	uint32_t buffer;
	uint32_t rate;
	struct _snd_pcm *pcm;
	struct _snd_ctl *control;
	int control_fd;
	int radio_fd;
	unsigned int headphone_id;
	unsigned int speaker_id;
	unsigned int jack_id;
	unsigned int fm_id;
	struct fplinux_audio_volume_control headphone_volume;
	struct fplinux_audio_volume_control speaker_volume;
	struct fplinux_audio_volume_control master_volume;
	unsigned int fm_volume;
	bool fixed_speaker;
	bool jack_present;
	bool fm_enabled;
	bool fm_muted;
	bool alarm_active;
	enum fplinux_audio_route route;
	enum fplinux_audio_route applied_route;
	struct pollfd fds[FPLINUX_AUDIO_DEVICE_FDS];
	unsigned int fd_count;
	/* Output frames written or dropped since the service started. */
	uint64_t written;
};

/* Discover only controls that the current card actually exposes. */
int fplinux_audio_device_init(struct fplinux_audio_device *device);
void fplinux_audio_device_cleanup(struct fplinux_audio_device *device);
int fplinux_audio_device_route(struct fplinux_audio_device *device,
			       enum fplinux_audio_route route);
/* Alarm may use a supported speaker while the selected private sink is absent. */
int fplinux_audio_device_alarm(struct fplinux_audio_device *device,
			       bool active);
/* Returns event bits, without switching routes, or a negative errno. */
int fplinux_audio_device_events(struct fplinux_audio_device *device);
/* Tune while muted, then claim or release the exclusive FM route. */
int fplinux_audio_device_tune(struct fplinux_audio_device *device,
			      unsigned int frequency);
bool fplinux_audio_device_fm_available(
	const struct fplinux_audio_device *device);
int fplinux_audio_device_fm(struct fplinux_audio_device *device, bool enabled,
			    unsigned int level);
int fplinux_audio_device_radio_close(struct fplinux_audio_device *device);

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
