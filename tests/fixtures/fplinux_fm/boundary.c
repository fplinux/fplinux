// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int frequency;
static snd_ctl_t control;

int open(const char *path, int flags, ...)
{
	(void)flags;
	if (strcmp(path, "/dev/radio0")) {
		errno = ENOENT;
		return -1;
	}
	fputs("RADIO_OPEN\n", stderr);
	return 17;
}

int close(int fd)
{
	if (fd == 17)
		fputs("RADIO_CLOSE\n", stderr);
	return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	void *data;

	(void)fd;
	va_start(arguments, request);
	data = va_arg(arguments, void *);
	va_end(arguments);
	if (request == VIDIOC_QUERYCAP) {
		struct v4l2_capability *capability = data;

		capability->capabilities = V4L2_CAP_RADIO |
					   V4L2_CAP_HW_FREQ_SEEK;
	} else if (request == VIDIOC_G_TUNER) {
		struct v4l2_tuner *tuner = data;

		tuner->type = V4L2_TUNER_RADIO;
		tuner->capability = V4L2_TUNER_CAP_LOW |
				    V4L2_TUNER_CAP_HWSEEK_BOUNDED;
		tuner->signal = 0;
	} else if (request == VIDIOC_S_FREQUENCY) {
		if (getenv("FM_TEST_FAIL_TUNE")) {
			errno = ETIMEDOUT;
			return -1;
		}
		frequency = ((struct v4l2_frequency *)data)->frequency;
		if (getenv("FM_TEST_EXPECT_983") && frequency != 983U * 1600U) {
			errno = EINVAL;
			return -1;
		}
	} else if (request == VIDIOC_S_HW_FREQ_SEEK) {
		const struct v4l2_hw_freq_seek *seek = data;

		if (seek->type != V4L2_TUNER_RADIO || !seek->seek_upward ||
		    seek->wrap_around || seek->spacing != 100000U ||
		    seek->rangelow != 875U * 1600U ||
		    seek->rangehigh != 1080U * 1600U) {
			errno = EINVAL;
			return -1;
		}
		if (getenv("FM_TEST_NO_CHANNEL")) {
			errno = ENODATA;
			return -1;
		}
		if (frequency <= 875U * 1600U)
			frequency = 875U * 1600U;
		else if (frequency <= 881U * 1600U)
			frequency = 881U * 1600U;
		else if (frequency <= 934U * 1600U)
			frequency = 934U * 1600U;
		else if (frequency <= 1080U * 1600U)
			frequency = 1080U * 1600U;
		else {
			errno = ENODATA;
			return -1;
		}
	} else if (request == VIDIOC_G_FREQUENCY) {
		((struct v4l2_frequency *)data)->frequency = frequency;
	} else if (request == VIDIOC_S_CTRL) {
		const struct v4l2_control *control_value = data;

		if (control_value->id != V4L2_CID_AUDIO_MUTE) {
			errno = EINVAL;
			return -1;
		}
		fputs(control_value->value ? "V4L2_MUTE\n" : "V4L2_UNMUTE\n",
		      stderr);
		if ((control_value->value &&
		     getenv("FM_TEST_FAIL_V4L2_MUTE")) ||
		    (!control_value->value &&
		     getenv("FM_TEST_FAIL_V4L2_UNMUTE"))) {
			errno = EIO;
			return -1;
		}
		if (!control_value->value && getenv("FM_TEST_SIGNAL"))
			raise(SIGTERM);
	} else {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int snd_ctl_open(snd_ctl_t **result, const char *name, int mode)
{
	(void)name;
	(void)mode;
	*result = &control;
	return 0;
}

int snd_ctl_close(snd_ctl_t *result)
{
	(void)result;
	return 0;
}

void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *id, int interface)
{
	(void)id;
	(void)interface;
}

void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *id, const char *name)
{
	(void)id;
	(void)name;
}

void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *value,
			       const snd_ctl_elem_id_t *id)
{
	(void)value;
	(void)id;
}

int snd_ctl_elem_read(snd_ctl_t *result, snd_ctl_elem_value_t *value)
{
	(void)result;
	value->enabled = getenv("FM_TEST_ALREADY_ACTIVE") != NULL;
	return 0;
}

int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index)
{
	(void)index;
	return value->enabled;
}

void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long enabled)
{
	(void)index;
	value->enabled = enabled;
}

int snd_ctl_elem_write(snd_ctl_t *result, snd_ctl_elem_value_t *value)
{
	(void)result;
	fputs(value->enabled ? "FM_ON\n" : "FM_OFF\n", stderr);
	if ((value->enabled && getenv("FM_TEST_FAIL_FM_ON")) ||
	    (!value->enabled && getenv("FM_TEST_FAIL_FM_OFF")))
		return -EIO;
	return 0;
}

const char *snd_strerror(int error)
{
	(void)error;
	return "test ALSA error";
}
