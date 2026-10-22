// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "audio-device.h"
#include "audio-mixer.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>

/* Applying the hardware parameters prepares the card: FM fails here. */
static int set_hardware(snd_pcm_t *pcm,
			const struct fplinux_audio_device *device)
{
	snd_pcm_hw_params_t *hw;
	int result;

	snd_pcm_hw_params_alloca(&hw);
	result = snd_pcm_hw_params_any(pcm, hw);
	if (result >= 0)
		result = snd_pcm_hw_params_set_access(
			pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (result >= 0)
		result = snd_pcm_hw_params_set_format(pcm, hw,
						      SND_PCM_FORMAT_S16_LE);
	if (result >= 0)
		result = snd_pcm_hw_params_set_channels(
			pcm, hw, FPLINUX_AUDIO_MIXER_CHANNELS);
	if (result >= 0)
		result = snd_pcm_hw_params_set_rate(
			pcm, hw, FPLINUX_AUDIO_MIXER_RATE, 0);
	if (result >= 0)
		result = snd_pcm_hw_params_set_period_size(pcm, hw,
							   device->period, 0);
	if (result >= 0)
		result = snd_pcm_hw_params_set_buffer_size(pcm, hw,
							   device->buffer);
	if (result >= 0)
		result = snd_pcm_hw_params(pcm, hw);
	return result;
}

/* Start once the buffer is full, and wake for each free period. */
static int set_software(snd_pcm_t *pcm,
			const struct fplinux_audio_device *device)
{
	snd_pcm_sw_params_t *sw;
	int result;

	snd_pcm_sw_params_alloca(&sw);
	result = snd_pcm_sw_params_current(pcm, sw);
	if (result >= 0)
		result = snd_pcm_sw_params_set_start_threshold(pcm, sw,
							       device->buffer);
	if (result >= 0)
		result = snd_pcm_sw_params_set_avail_min(pcm, sw,
							 device->period);
	if (result >= 0)
		result = snd_pcm_sw_params(pcm, sw);
	return result;
}

int fplinux_audio_device_open(struct fplinux_audio_device *device)
{
	snd_pcm_t *pcm;
	int result;

	result = snd_pcm_open(&pcm, device->name, SND_PCM_STREAM_PLAYBACK,
			      SND_PCM_NONBLOCK);
	if (result < 0)
		return result;
	result = set_hardware(pcm, device);
	if (result >= 0)
		result = set_software(pcm, device);
	if (result >= 0) {
		result = snd_pcm_poll_descriptors_count(pcm);
		if (result <= 0 || result > (int)FPLINUX_AUDIO_DEVICE_FDS)
			result = -EINVAL;
	}
	if (result >= 0)
		result = snd_pcm_poll_descriptors(pcm, device->fds,
						  (unsigned int)result);
	if (result <= 0) {
		snd_pcm_close(pcm);
		return result ? result : -EINVAL;
	}
	device->fd_count = (unsigned int)result;
	device->pcm = pcm;
	return 0;
}

void fplinux_audio_device_close(struct fplinux_audio_device *device)
{
	snd_pcm_close(device->pcm);
	device->pcm = NULL;
	device->fd_count = 0;
}

bool fplinux_audio_device_ready(struct fplinux_audio_device *device,
				struct pollfd *fds)
{
	unsigned short revents = 0;

	if (snd_pcm_poll_descriptors_revents(device->pcm, fds, device->fd_count,
					     &revents) < 0)
		return true;
	return revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL);
}

long fplinux_audio_device_avail(struct fplinux_audio_device *device)
{
	return snd_pcm_avail_update(device->pcm);
}

long fplinux_audio_device_write(struct fplinux_audio_device *device,
				const int16_t *frames, uint32_t count)
{
	snd_pcm_sframes_t written = snd_pcm_writei(device->pcm, frames, count);

	if (written > 0)
		device->written += (uint64_t)written;
	return written;
}

int fplinux_audio_device_recover(struct fplinux_audio_device *device)
{
	return snd_pcm_prepare(device->pcm);
}

uint64_t fplinux_audio_device_played(struct fplinux_audio_device *device)
{
	snd_pcm_sframes_t delay = 0;

	/* A card that cannot tell has played or lost everything. */
	if (snd_pcm_delay(device->pcm, &delay) < 0 || delay < 0)
		delay = 0;
	if ((uint64_t)delay > device->written)
		return 0;
	return device->written - (uint64_t)delay;
}

const char *fplinux_audio_device_error(int error)
{
	return snd_strerror(error);
}
