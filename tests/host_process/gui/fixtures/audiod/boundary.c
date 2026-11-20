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
 * - FPLINUX_AUDIOD_FAKE_HOLD_PCM: while this file exists the card has
 *   no space for another period; log "pcm hold" when first observed;
 * - FPLINUX_AUDIOD_FAKE_ABSENT: when set, playback PCM is absent;
 * - FPLINUX_AUDIOD_FAKE_XRUN: the write with this number, counted from 1,
 *   fails with an underrun;
 * - FPLINUX_AUDIOD_FAKE_LEVELS: record the configured speaker, headphone
 *   left and headphone right levels when the control device closes.
 */
#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define RATE 48000U

struct _snd_pcm {
	int timer;
	FILE *played;
	snd_pcm_hw_params_t hardware;
	snd_pcm_sw_params_t software;
	uint64_t written;
	bool started;
	bool held;
	uint64_t start_ns;
};

static struct _snd_pcm card = { .timer = -1 };
static bool card_open;
static unsigned long writes;

/* A headphone route without a jack sensor; playback remains the paced card. */
struct _snd_ctl {
	int events;
	bool headphone_on;
	bool speaker_on;
	bool fm_on;
	long headphone_level[2];
	long speaker_level;
	long master_level;
};
static struct _snd_ctl control_card;
static int radio_fd = -1;
static void event(const char *format, ...);

enum {
	HEADPHONE_SWITCH = 1,
	SPEAKER_SWITCH,
	FM_SWITCH,
	HEADPHONE_VOLUME,
	SPEAKER_VOLUME,
	MASTER_VOLUME,
};

static bool gain_error(unsigned int id, const char *operation)
{
	const char *failure = getenv("FPLINUX_AUDIOD_FAKE_GAIN_ERROR");

	return id == SPEAKER_VOLUME && failure && !strcmp(failure, operation);
}

int access(const char *path, int mode)
{
	if (!strcmp(path, "/dev/radio0")) {
		path = getenv("FPLINUX_AUDIOD_FAKE_RADIO_READY");
		if (!path) {
			errno = ENOENT;
			return -1;
		}
	}
	return faccessat(AT_FDCWD, path, mode, 0);
}

int open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/radio0")) {
		if (access(path, F_OK) < 0)
			return -1;
		radio_fd = openat(AT_FDCWD, "/dev/null", flags);
		event("radio open");
		return radio_fd;
	}
	if (flags & O_CREAT) {
		va_list arguments;
		mode_t mode;

		va_start(arguments, flags);
		mode = (mode_t)va_arg(arguments, int);
		va_end(arguments);
		return openat(AT_FDCWD, path, flags, mode);
	}
	return openat(AT_FDCWD, path, flags);
}

#ifdef __GLIBC__
int ioctl(int fd, unsigned long request, ...)
#else
int ioctl(int fd, int request, ...)
#endif
{
	va_list arguments;
	void *data;
	unsigned int operation = (unsigned int)request;

	if (fd != radio_fd) {
		errno = ENOTTY;
		return -1;
	}
	va_start(arguments, request);
	data = va_arg(arguments, void *);
	va_end(arguments);
	if (operation == VIDIOC_QUERYCAP)
		((struct v4l2_capability *)data)->capabilities = V4L2_CAP_RADIO;
	else if (operation == VIDIOC_G_TUNER) {
		struct v4l2_tuner *tuner = data;

		tuner->type = V4L2_TUNER_RADIO;
		tuner->capability = V4L2_TUNER_CAP_LOW;
	} else if (operation == VIDIOC_S_FREQUENCY) {
		struct v4l2_frequency *frequency = data;

		event("radio tune %u", frequency->frequency / 1600U);
	} else if (operation == VIDIOC_S_CTRL) {
		const struct v4l2_control *control = data;

		if (control->id != V4L2_CID_AUDIO_MUTE) {
			errno = EINVAL;
			return -1;
		}
		event("radio mute %d", control->value);
		if (!control->value)
			event("radio levels %ld %ld %ld",
			      control_card.speaker_level,
			      control_card.headphone_level[0],
			      control_card.headphone_level[1]);
	} else {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int snd_ctl_open(snd_ctl_t **control, const char *name, int mode)
{
	(void)name;
	if (mode != SND_CTL_NONBLOCK)
		return -EINVAL;
	control_card = (struct _snd_ctl){
		.headphone_on = true,
		.headphone_level = { 3, 7 },
		.speaker_level = 6,
		.master_level = 3,
	};
	if (getenv("FPLINUX_AUDIOD_FAKE_NO_PROFILE"))
		control_card.headphone_level[1] = 5;
	control_card.events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (control_card.events < 0)
		return -errno;
	*control = &control_card;
	return 0;
}

int snd_ctl_close(snd_ctl_t *control)
{
	const char *path = getenv("FPLINUX_AUDIOD_FAKE_LEVELS");
	FILE *levels;

	if (path && (levels = fopen(path, "w"))) {
		fprintf(levels, "%ld %ld %ld\n", control->speaker_level,
			control->headphone_level[0],
			control->headphone_level[1]);
		fclose(levels);
	}
	if (getenv("FPLINUX_AUDIOD_FAKE_QIN"))
		event("master final %ld", control->master_level);
	return close(control->events);
}

int snd_ctl_card_info(snd_ctl_t *control, snd_ctl_card_info_t *info)
{
	(void)control;
	(void)info;
	return 0;
}

const char *snd_ctl_card_info_get_driver(const snd_ctl_card_info_t *info)
{
	(void)info;
	return getenv("FPLINUX_AUDIOD_FAKE_QIN") ? "SC9820E audio" :
						   "ums9117-pcm";
}

void snd_ctl_elem_info_set_interface(snd_ctl_elem_info_t *info,
				     snd_ctl_elem_iface_t iface)
{
	info->iface = iface;
}

void snd_ctl_elem_info_set_name(snd_ctl_elem_info_t *info, const char *name)
{
	info->name = name;
}

int snd_ctl_elem_info(snd_ctl_t *control, snd_ctl_elem_info_t *info)
{
	(void)control;
	if (info->iface != SND_CTL_ELEM_IFACE_MIXER)
		return -ENOENT;
	if (getenv("FPLINUX_AUDIOD_FAKE_QIN")) {
		if (strcmp(info->name, "Master Playback Volume"))
			return -ENOENT;
		info->numid = MASTER_VOLUME;
		return 0;
	}
	if (getenv("FPLINUX_AUDIOD_FAKE_NO_PROFILE") &&
	    (!strcmp(info->name, "Speaker Playback Switch") ||
	     !strcmp(info->name, "Speaker Playback Volume")))
		return -ENOENT;
	if (!strcmp(info->name, "Headphone Playback Switch"))
		info->numid = HEADPHONE_SWITCH;
	else if (!strcmp(info->name, "Speaker Playback Switch"))
		info->numid = SPEAKER_SWITCH;
	else if (!strcmp(info->name, "FM Playback Switch"))
		info->numid = FM_SWITCH;
	else if (!strcmp(info->name, "Headphone Playback Volume"))
		info->numid = HEADPHONE_VOLUME;
	else if (!strcmp(info->name, "Speaker Playback Volume"))
		info->numid = SPEAKER_VOLUME;
	else
		return -ENOENT;
	if (gain_error(info->numid, "info"))
		return -EIO;
	return 0;
}

int snd_ctl_elem_info_get_type(const snd_ctl_elem_info_t *info)
{
	if (gain_error(info->numid, "type"))
		return SND_CTL_ELEM_TYPE_BOOLEAN;
	return info->numid >= HEADPHONE_VOLUME ? SND_CTL_ELEM_TYPE_INTEGER :
						 SND_CTL_ELEM_TYPE_BOOLEAN;
}

int snd_ctl_elem_info_is_readable(const snd_ctl_elem_info_t *info)
{
	return !gain_error(info->numid, "unreadable");
}

int snd_ctl_elem_info_is_writable(const snd_ctl_elem_info_t *info)
{
	return !gain_error(info->numid, "readonly");
}

unsigned int snd_ctl_elem_info_get_count(const snd_ctl_elem_info_t *info)
{
	if (gain_error(info->numid, "count"))
		return 3;
	return info->numid == HEADPHONE_VOLUME ? 2 : 1;
}

long snd_ctl_elem_info_get_min(const snd_ctl_elem_info_t *info)
{
	return gain_error(info->numid, "minimum") ? -1 : 0;
}

long snd_ctl_elem_info_get_max(const snd_ctl_elem_info_t *info)
{
	if (gain_error(info->numid, "maximum"))
		return 0;
	if (info->numid == MASTER_VOLUME)
		return 7;
	if (info->numid == HEADPHONE_VOLUME &&
	    getenv("FPLINUX_AUDIOD_FAKE_NO_PROFILE"))
		return 6;
	return info->numid >= HEADPHONE_VOLUME ? 9 : 1;
}

long snd_ctl_elem_value_get_integer(const snd_ctl_elem_value_t *value,
				    unsigned int index)
{
	return value->integer[index];
}

void snd_ctl_elem_value_set_integer(snd_ctl_elem_value_t *value,
				    unsigned int index, long level)
{
	value->integer[index] = level;
}

unsigned int snd_ctl_elem_info_get_numid(const snd_ctl_elem_info_t *info)
{
	return info->numid;
}

void snd_ctl_elem_value_set_numid(snd_ctl_elem_value_t *value,
				  unsigned int numid)
{
	value->numid = numid;
}

int snd_ctl_elem_read(snd_ctl_t *control, snd_ctl_elem_value_t *value)
{
	if (gain_error(value->numid, "read"))
		return -EIO;
	switch (value->numid) {
	case HEADPHONE_SWITCH:
		value->boolean = control->headphone_on;
		break;
	case SPEAKER_SWITCH:
		value->boolean = control->speaker_on;
		break;
	case FM_SWITCH:
		value->boolean = control->fm_on;
		break;
	case HEADPHONE_VOLUME:
		value->integer[0] = control->headphone_level[0];
		value->integer[1] = control->headphone_level[1];
		break;
	case SPEAKER_VOLUME:
		value->integer[0] = control->speaker_level;
		break;
	case MASTER_VOLUME:
		value->integer[0] = control->master_level;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index)
{
	(void)index;
	return value->boolean != 0;
}

void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long enabled)
{
	(void)index;
	value->boolean = enabled;
}

int snd_ctl_elem_write(snd_ctl_t *control, snd_ctl_elem_value_t *value)
{
	const char *failure_path = getenv("FPLINUX_AUDIOD_FAKE_FAIL_GAIN");
	FILE *failure;
	long failed_level;

	if (gain_error(value->numid, "write") && value->integer[0] == 9)
		return -EIO;
	if (value->numid >= HEADPHONE_VOLUME && failure_path &&
	    (failure = fopen(failure_path, "r"))) {
		bool failed = fscanf(failure, "%ld", &failed_level) == 1 &&
			      failed_level == value->integer[0];

		fclose(failure);
		if (failed)
			return -EIO;
	}
	switch (value->numid) {
	case HEADPHONE_SWITCH:
		control->headphone_on = value->boolean;
		break;
	case SPEAKER_SWITCH:
		control->speaker_on = value->boolean;
		break;
	case FM_SWITCH:
		if (value->boolean && card_open)
			return -EBUSY;
		if (control->fm_on != (value->boolean != 0))
			event(value->boolean ? "fm on" : "fm off");
		control->fm_on = value->boolean;
		break;
	case HEADPHONE_VOLUME:
		if (control->headphone_level[0] != value->integer[0] ||
		    control->headphone_level[1] != value->integer[1])
			event("headphone volume %ld %ld", value->integer[0],
			      value->integer[1]);
		control->headphone_level[0] = value->integer[0];
		control->headphone_level[1] = value->integer[1];
		break;
	case SPEAKER_VOLUME:
		if (control->speaker_level != value->integer[0])
			event("speaker volume %ld", value->integer[0]);
		control->speaker_level = value->integer[0];
		break;
	case MASTER_VOLUME:
		if (control->master_level != value->integer[0])
			event("master volume %ld", value->integer[0]);
		control->master_level = value->integer[0];
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

int snd_ctl_subscribe_events(snd_ctl_t *control, int subscribe)
{
	(void)control;
	return subscribe == 1 ? 0 : -EINVAL;
}

int snd_ctl_poll_descriptors_count(snd_ctl_t *control)
{
	(void)control;
	return 1;
}

int snd_ctl_poll_descriptors(snd_ctl_t *control, struct pollfd *fds,
			     unsigned int count)
{
	if (count != 1)
		return -EINVAL;
	fds[0] = (struct pollfd){ .fd = control->events, .events = POLLIN };
	return 1;
}

int snd_ctl_read(snd_ctl_t *control, snd_ctl_event_t *event)
{
	(void)control;
	(void)event;
	return -EAGAIN;
}

int snd_ctl_event_get_type(const snd_ctl_event_t *event)
{
	(void)event;
	return SND_CTL_EVENT_ELEM;
}

unsigned int snd_ctl_event_elem_get_numid(const snd_ctl_event_t *event)
{
	(void)event;
	return 0;
}

unsigned int snd_ctl_event_elem_get_mask(const snd_ctl_event_t *event)
{
	(void)event;
	return 0;
}

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
	frames = (now - pcm->start_ns) * pcm->hardware.rate / 1000000000U;
	if (frames <= pcm->written)
		return frames;
	pcm->start_ns = now - pcm->written * 1000000000U / pcm->hardware.rate;
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
	if (card_open || control_card.fm_on ||
	    (busy && access(busy, F_OK) == 0)) {
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
	if (getenv("FPLINUX_AUDIOD_FAKE_QIN"))
		event("pcm master %ld", control_card.master_level);
	else
		event("pcm levels %ld %ld %ld", control_card.speaker_level,
		      control_card.headphone_level[0],
		      control_card.headphone_level[1]);
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
	    (params->rate != RATE && params->rate != 44100) ||
	    !params->period || params->buffer < 2 * params->period)
		return -EINVAL;
	pcm->hardware = *params;
	event("hw %u %u %lu %lu", params->rate, params->channels,
	      params->period, params->buffer);
	period_ns = (uint64_t)params->period * 1000000000U / params->rate;
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
	const char *hold = getenv("FPLINUX_AUDIOD_FAKE_HOLD_PCM");

	if (hold && access(hold, F_OK) == 0) {
		if (!pcm->held)
			event("pcm hold");
		pcm->held = true;
		return 0;
	}
	pcm->held = false;
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
	if (getenv("FPLINUX_AUDIOD_FAKE_SIGNAL_EVENTS")) {
		const int16_t *samples = frames;

		for (size_t i = 0; i < count * pcm->hardware.channels; i++) {
			if (samples[i]) {
				event("pcm signal");
				break;
			}
		}
	}
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
