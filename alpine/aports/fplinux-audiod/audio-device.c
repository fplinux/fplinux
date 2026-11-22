// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "audio-device.h"
#include "fplinux-audio-radio.h"
#include "audio-mixer.h"

#include <fplinux-audio-protocol.h>

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int radio_mute(struct fplinux_audio_device *device, bool muted);

static int save_volume(struct fplinux_audio_device *device,
		       struct fplinux_audio_volume_control *volume,
		       const char *name, unsigned int count)
{
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	unsigned int channel;
	long maximum;
	int result;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	result = snd_ctl_elem_info(device->control, info);
	if (result == -ENOENT)
		return 0;
	if (result < 0)
		return result;
	if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER ||
	    snd_ctl_elem_info_get_count(info) != count)
		return -EINVAL;
	maximum = snd_ctl_elem_info_get_max(info);
	if (snd_ctl_elem_info_get_min(info) != 0 || maximum < 1)
		return -EINVAL;
	if (!snd_ctl_elem_info_is_readable(info) ||
	    !snd_ctl_elem_info_is_writable(info))
		return -EPERM;
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_value_set_numid(value, snd_ctl_elem_info_get_numid(info));
	result = snd_ctl_elem_read(device->control, value);
	if (result < 0)
		return result;
	for (channel = 0; channel < count; ++channel)
		volume->saved[channel] =
			snd_ctl_elem_value_get_integer(value, channel);
	volume->count = count;
	volume->maximum = maximum;
	volume->id = snd_ctl_elem_info_get_numid(info);
	return 0;
}

static int write_volume(struct fplinux_audio_device *device,
			const struct fplinux_audio_volume_control *volume,
			const long *levels)
{
	snd_ctl_elem_value_t *value;
	unsigned int channel;

	if (!volume->id)
		return 0;
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_value_set_numid(value, volume->id);
	for (channel = 0; channel < volume->count; ++channel)
		snd_ctl_elem_value_set_integer(value, channel, levels[channel]);
	return snd_ctl_elem_write(device->control, value);
}

static int set_volume(struct fplinux_audio_device *device,
		      const struct fplinux_audio_volume_control *volume,
		      long level)
{
	long levels[2] = { level, level };

	return write_volume(device, volume, levels);
}

/* PCM classes share one hardware reference and keep their own mixer gain. */
static int prepare_pcm_volume(struct fplinux_audio_device *device)
{
	int result = set_volume(device, &device->headphone_volume,
				device->headphone_volume.maximum);

	if (result >= 0)
		result = set_volume(device, &device->speaker_volume,
				    device->speaker_volume.maximum);
	if (result >= 0)
		result = set_volume(device, &device->master_volume,
				    device->master_volume.maximum);
	return result;
}

/* FM bypasses the mixer: map its nonzero levels to hardware ordinals. */
static int set_fm_volume(struct fplinux_audio_device *device,
			 enum fplinux_audio_route route, unsigned int level)
{
	const struct fplinux_audio_volume_control *volume =
		device->master_volume.id ? &device->master_volume :
		route == FPLINUX_AUDIO_ROUTE_SPEAKER ?
					   &device->speaker_volume :
					   &device->headphone_volume;
	long span, steps = FPLINUX_AUDIO_VOLUME_MAX - 1;
	long position = level ? level - 1 : 0;
	long ordinal = 0;

	if (!volume->id)
		return 0;
	span = volume->maximum - 1;
	/* Divide first so the declared long range cannot overflow the scale. */
	if (level)
		ordinal = 1 + span / steps * position +
			  span % steps * position / steps;
	return set_volume(device, volume, ordinal);
}

static unsigned int control_id(snd_ctl_t *control, const char *name,
			       snd_ctl_elem_iface_t iface)
{
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_interface(info, iface);
	snd_ctl_elem_info_set_name(info, name);
	if (snd_ctl_elem_info(control, info) < 0 ||
	    snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN)
		return 0;
	return snd_ctl_elem_info_get_numid(info);
}

static int read_switch(struct fplinux_audio_device *device, unsigned int id,
		       bool *enabled)
{
	snd_ctl_elem_value_t *value;
	int result;

	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_value_set_numid(value, id);
	result = snd_ctl_elem_read(device->control, value);
	if (result >= 0)
		*enabled = snd_ctl_elem_value_get_boolean(value, 0) != 0;
	return result;
}

static int set_switch(struct fplinux_audio_device *device, unsigned int id,
		      bool enabled)
{
	snd_ctl_elem_value_t *value;

	if (!id)
		return -ENOTSUP;
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_value_set_numid(value, id);
	snd_ctl_elem_value_set_boolean(value, 0, enabled);
	return snd_ctl_elem_write(device->control, value);
}

int fplinux_audio_device_route(struct fplinux_audio_device *device,
			       enum fplinux_audio_route route)
{
	enum fplinux_audio_route applied = route;
	unsigned int enable, disable;
	bool muted = device->fm_muted;
	int result;

	if (applied == FPLINUX_AUDIO_ROUTE_AUTO)
		applied = device->jack_id && device->jack_present ?
				  FPLINUX_AUDIO_ROUTE_HEADPHONES :
			  device->speaker_id || device->fixed_speaker ?
				  FPLINUX_AUDIO_ROUTE_SPEAKER :
				  FPLINUX_AUDIO_ROUTE_HEADPHONES;
	if (device->alarm_active && applied == FPLINUX_AUDIO_ROUTE_HEADPHONES &&
	    device->jack_id && !device->jack_present) {
		if (!device->speaker_id)
			return -ENODEV;
		applied = FPLINUX_AUDIO_ROUTE_SPEAKER;
	}
	if (device->applied_route == applied) {
		device->route = route;
		return 0;
	}
	if (applied == FPLINUX_AUDIO_ROUTE_SPEAKER && device->fixed_speaker) {
		device->route = route;
		device->applied_route = applied;
		return 0;
	}
	enable = applied == FPLINUX_AUDIO_ROUTE_SPEAKER ? device->speaker_id :
							  device->headphone_id;
	disable = applied == FPLINUX_AUDIO_ROUTE_SPEAKER ?
			  device->headphone_id :
			  device->speaker_id;
	if (device->fm_enabled) {
		result = radio_mute(device, true);
		if (result < 0)
			return result;
		device->fm_muted = true;
		result = set_fm_volume(device, applied, device->fm_volume);
		if (result < 0) {
			if (radio_mute(device, muted) >= 0)
				device->fm_muted = muted;
			return result;
		}
	}
	/* Enable first: the card can switch from its idle path without a pop. */
	result = set_switch(device, enable, true);
	if (result >= 0 && disable)
		result = set_switch(device, disable, false);
	if (result < 0)
		return result;
	device->route = route;
	device->applied_route = applied;
	if (device->fm_enabled && !muted) {
		result = radio_mute(device, false);
		if (result < 0)
			return result;
		device->fm_muted = false;
	}
	return 0;
}

int fplinux_audio_device_alarm(struct fplinux_audio_device *device, bool active)
{
	bool previous = device->alarm_active;
	int result;

	if (previous == active)
		return 0;
	device->alarm_active = active;
	result = fplinux_audio_device_route(device, device->route);
	if (result < 0)
		device->alarm_active = previous;
	return result;
}

int fplinux_audio_device_init(struct fplinux_audio_device *device)
{
	struct pollfd fd;
	bool fm_enabled = false;
	int result;

	device->control_fd = -1;
	device->radio_fd = -1;
	device->fm_muted = true;
	result = snd_ctl_open(&device->control, "default", SND_CTL_NONBLOCK);
	if (result < 0)
		return result;
	device->headphone_id = control_id(device->control,
					  "Headphone Playback Switch",
					  SND_CTL_ELEM_IFACE_MIXER);
	device->speaker_id = control_id(device->control,
					"Speaker Playback Switch",
					SND_CTL_ELEM_IFACE_MIXER);
	device->jack_id = control_id(device->control, "Headphone Jack",
				     SND_CTL_ELEM_IFACE_CARD);
	device->fm_id = control_id(device->control, "FM Playback Switch",
				   SND_CTL_ELEM_IFACE_MIXER);
	if (device->fm_id) {
		result = read_switch(device, device->fm_id, &fm_enabled);
		if (result < 0)
			goto failed;
		if (fm_enabled &&
		    (result = set_switch(device, device->fm_id, false)) < 0)
			goto failed;
	}
	/* Save every channel before changing any of the borrowed controls. */
	result = save_volume(device, &device->headphone_volume,
			     "Headphone Playback Volume", 2);
	if (result >= 0)
		result = save_volume(device, &device->speaker_volume,
				     "Speaker Playback Volume", 1);
	if (result >= 0)
		result = save_volume(device, &device->master_volume,
				     "Master Playback Volume", 1);
	if (result >= 0)
		result = prepare_pcm_volume(device);
	if (result < 0)
		goto failed;
	result = snd_ctl_subscribe_events(device->control, 1);
	if (result < 0)
		goto failed;
	if (snd_ctl_poll_descriptors_count(device->control) != 1 ||
	    snd_ctl_poll_descriptors(device->control, &fd, 1) != 1) {
		result = -EINVAL;
		goto failed;
	}
	device->control_fd = fd.fd;
	if (device->jack_id &&
	    (result = read_switch(device, device->jack_id,
				  &device->jack_present)) < 0)
		goto failed;
	result = fplinux_audio_device_route(device, FPLINUX_AUDIO_ROUTE_AUTO);
	if (result >= 0)
		return result;
failed:
	fplinux_audio_device_cleanup(device);
	return result;
}

int fplinux_audio_device_events(struct fplinux_audio_device *device)
{
	snd_ctl_event_t *event;
	int result, changes = 0;

	snd_ctl_event_alloca(&event);
	while ((result = snd_ctl_read(device->control, event)) > 0) {
		bool was_present = device->jack_present;

		if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM ||
		    snd_ctl_event_elem_get_numid(event) != device->jack_id)
			continue;
		if (snd_ctl_event_elem_get_mask(event) ==
		    SND_CTL_EVENT_MASK_REMOVE)
			return -ENODEV;
		result = read_switch(device, device->jack_id,
				     &device->jack_present);
		if (result < 0)
			return result;
		if (was_present != device->jack_present)
			changes |= FPLINUX_AUDIO_DEVICE_JACK_CHANGED;
		if (was_present && !device->jack_present)
			changes |= FPLINUX_AUDIO_DEVICE_PRIVATE_LOST;
	}
	if (result < 0 && result != -EAGAIN)
		return result;
	return changes;
}

static int radio_mute(struct fplinux_audio_device *device, bool muted)
{
	struct v4l2_control control = {
		.id = V4L2_CID_AUDIO_MUTE,
		.value = muted,
	};

	return ioctl(device->radio_fd, VIDIOC_S_CTRL, &control) < 0 ? -errno :
								      0;
}

bool fplinux_audio_device_fm_available(const struct fplinux_audio_device *device)
{
	return device->fm_id && access("/dev/radio0", F_OK) == 0;
}

int fplinux_audio_device_tune(struct fplinux_audio_device *device,
			      unsigned int frequency)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_tuner tuner = { .index = 0 };
	struct v4l2_frequency tune = {
		.type = V4L2_TUNER_RADIO,
		.frequency = frequency * 1600U,
	};
	unsigned int capabilities;
	int result;

	if (!device->fm_id || frequency < FPLINUX_AUDIO_FM_LOW_100KHZ ||
	    frequency > FPLINUX_AUDIO_FM_HIGH_100KHZ)
		return -ENOTSUP;
	device->radio_fd = open("/dev/radio0", O_RDWR | O_CLOEXEC);
	if (device->radio_fd < 0)
		return -errno;
	if (ioctl(device->radio_fd, VIDIOC_QUERYCAP, &capability) < 0 ||
	    ioctl(device->radio_fd, VIDIOC_G_TUNER, &tuner) < 0)
		return -errno;
	capabilities = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
			       capability.device_caps :
			       capability.capabilities;
	if (!(capabilities & V4L2_CAP_RADIO) ||
	    tuner.type != V4L2_TUNER_RADIO ||
	    !(tuner.capability & V4L2_TUNER_CAP_LOW))
		return -ENOTSUP;
	result = radio_mute(device, true);
	device->fm_muted = true;
	if (result >= 0 &&
	    ioctl(device->radio_fd, VIDIOC_S_FREQUENCY, &tune) < 0)
		result = -errno;
	return result;
}

int fplinux_audio_device_fm(struct fplinux_audio_device *device, bool enabled,
			    unsigned int level)
{
	int result;

	if (level > FPLINUX_AUDIO_VOLUME_MAX)
		return -EINVAL;
	if (device->radio_fd < 0)
		return enabled ? -ENODEV : 0;
	if (enabled && device->pcm)
		return -EBUSY;
	if (device->fm_enabled == enabled &&
	    (!enabled ||
	     (device->fm_volume == level && device->fm_muted == (level == 0))))
		return 0;
	result = radio_mute(device, true);
	if (result >= 0)
		device->fm_muted = true;
	if (!enabled) {
		int switched = set_switch(device, device->fm_id, false);

		if (switched >= 0) {
			device->fm_enabled = false;
			switched = prepare_pcm_volume(device);
		}
		return result < 0 ? result : switched;
	}
	if (result < 0)
		return result;
	result = set_fm_volume(device, device->applied_route, level);
	if (result < 0)
		return result;
	result = set_switch(device, device->fm_id, enabled);
	if (result < 0)
		return result;
	device->fm_enabled = enabled;
	if (level) {
		result = radio_mute(device, false);
		if (result < 0)
			return result;
		device->fm_muted = false;
	}
	device->fm_volume = level;
	return result;
}

int fplinux_audio_device_radio_close(struct fplinux_audio_device *device)
{
	int result = fplinux_audio_device_fm(device, false, 0);

	if (device->radio_fd >= 0)
		close(device->radio_fd);
	device->radio_fd = -1;
	return result;
}

void fplinux_audio_device_cleanup(struct fplinux_audio_device *device)
{
	fplinux_audio_device_radio_close(device);
	if (device->pcm)
		fplinux_audio_device_close(device);
	if (device->control) {
		write_volume(device, &device->master_volume,
			     device->master_volume.saved);
		write_volume(device, &device->speaker_volume,
			     device->speaker_volume.saved);
		write_volume(device, &device->headphone_volume,
			     device->headphone_volume.saved);
		snd_ctl_close(device->control);
	}
	device->control = NULL;
}

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
		result = snd_pcm_hw_params_set_rate(pcm, hw, device->rate, 0);
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

	if (device->fm_enabled)
		return -EBUSY;
	result = prepare_pcm_volume(device);
	if (result < 0)
		return result;
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
