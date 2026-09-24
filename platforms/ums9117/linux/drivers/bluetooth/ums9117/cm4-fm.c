// SPDX-License-Identifier: GPL-2.0-only
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "cm4-fm.h"
#include "cm4-hci.h"

#define UMS9117_FM_CONFIG_BYTES 128
#define UMS9117_FM_MIN_10KHZ 8750
#define UMS9117_FM_MAX_10KHZ 10800
/* V4L2_TUNER_CAP_LOW uses 62.5 Hz, while CM4 uses 10 kHz. */
#define UMS9117_FM_FREQ_SCALE 160

enum fm_state {
	FM_OFF,
	FM_ON,
	FM_UNKNOWN,
};

struct ums9117_fm {
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *mute;
	struct mutex lock;
	const char *config_name;
	enum fm_state state;
	u16 frequency_10khz;
	bool opened;
};

static int unknown_state(struct ums9117_fm *radio, u8 subcommand, int error)
{
	radio->state = FM_UNKNOWN;
	ums9117_hci_fm_quarantine(error);
	dev_err(radio->v4l2.dev,
		"FM command 0x%02x failed: %pe; state unknown, close the audio route and cold boot\n",
		subcommand, ERR_PTR(error));
	return error;
}

static int command(struct ums9117_fm *radio, u8 subcommand, const u8 *payload,
		   size_t payload_bytes, u8 *reply, size_t reply_bytes)
{
	int ret;

	ret = ums9117_hci_fm_command(subcommand, payload, payload_bytes, reply,
				     reply_bytes, false);
	if (ret < 0)
		return unknown_state(radio, subcommand, ret);
	if (reply[6]) {
		dev_err(radio->v4l2.dev,
			"FM request 0x%02x rejected: status=0x%02x\n",
			subcommand, reply[6]);
		return unknown_state(radio, subcommand, -EREMOTEIO);
	}
	if (ret != reply_bytes)
		return unknown_state(radio, subcommand, -EPROTO);
	return 0;
}

static int set_byte(struct ums9117_fm *radio, u8 subcommand, u8 value)
{
	u8 reply[8];
	int ret;

	ret = command(radio, subcommand, &value, 1, reply, sizeof(reply));
	if (!ret && reply[7] != value)
		ret = unknown_state(radio, subcommand, -EPROTO);
	return ret;
}

static int enable(struct ums9117_fm *radio)
{
	const struct firmware *config;
	u8 payload[2 + UMS9117_FM_CONFIG_BYTES];
	u8 reply[7];
	int ret;

	if (radio->state == FM_UNKNOWN)
		return -EIO;
	if (radio->state == FM_ON)
		return 0;
	ret = request_firmware(&config, radio->config_name, radio->v4l2.dev);
	if (ret)
		return ret;
	if (config->size != UMS9117_FM_CONFIG_BYTES) {
		dev_err(radio->v4l2.dev,
			"invalid FM configuration %s: size=%zu, expected=%u\n",
			radio->config_name, config->size,
			UMS9117_FM_CONFIG_BYTES);
		release_firmware(config);
		return -EINVAL;
	}
	put_unaligned_le16(UMS9117_FM_MIN_10KHZ, payload);
	memcpy(payload + 2, config->data, UMS9117_FM_CONFIG_BYTES);
	release_firmware(config);
	ret = ums9117_hci_fm_hold();
	if (ret)
		return ret;
	ret = command(radio, 0x00, payload, sizeof(payload), reply,
		      sizeof(reply));
	if (ret)
		return ret;
	radio->state = FM_ON;
	/* ENABLE may unmute at its initial channel; userspace keeps ALSA closed. */
	ret = set_byte(radio, 0x02, 1);
	if (!ret)
		ret = set_byte(radio, 0x1b, 0);
	return ret;
}

static int disable(struct ums9117_fm *radio)
{
	u8 payload = 0;
	u8 reply[7];
	int ret;

	if (radio->state == FM_OFF)
		return 0;
	/* An unbounded CM4 tune/seek cannot be cancelled by another command. */
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	ret = set_byte(radio, 0x02, 1);
	if (!ret)
		ret = command(radio, 0x12, &payload, 1, reply, sizeof(reply));
	if (!ret) {
		radio->state = FM_OFF;
		radio->frequency_10khz = 0;
		ums9117_hci_fm_release();
	}
	return ret;
}

static int tune(struct ums9117_fm *radio, u16 frequency_10khz)
{
	u8 payload[2];
	u8 reply[11];
	int ret;

	put_unaligned_le16(frequency_10khz, payload);
	ret = command(radio, 0x01, payload, sizeof(payload), reply,
		      sizeof(reply));
	if (ret)
		return ret;
	if (get_unaligned_le16(reply + 9) != frequency_10khz)
		return unknown_state(radio, 0x01, -EPROTO);
	radio->frequency_10khz = frequency_10khz;
	return 0;
}

static int querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "ums9117-fm", sizeof(cap->driver));
	strscpy(cap->card, "UMS9117 FM receiver", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:ums9117-cm4", sizeof(cap->bus_info));
	return 0;
}

static int get_tuner(struct file *file, void *priv, struct v4l2_tuner *tuner)
{
	if (tuner->index)
		return -EINVAL;
	strscpy(tuner->name, "FM", sizeof(tuner->name));
	tuner->type = V4L2_TUNER_RADIO;
	tuner->capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO |
			    V4L2_TUNER_CAP_HWSEEK_BOUNDED;
	tuner->rangelow = UMS9117_FM_MIN_10KHZ * UMS9117_FM_FREQ_SCALE;
	tuner->rangehigh = UMS9117_FM_MAX_10KHZ * UMS9117_FM_FREQ_SCALE;
	tuner->audmode = V4L2_TUNER_MODE_STEREO;
	/* CM4's response bytes are not calibrated signal or stereo detection. */
	tuner->signal = 0;
	tuner->rxsubchans = 0;
	tuner->afc = 0;
	return 0;
}

static int set_tuner(struct file *file, void *priv,
		     const struct v4l2_tuner *tuner)
{
	/* The firmware's automatic stereo/mono selection remains in control. */
	return tuner->index ? -EINVAL : 0;
}

static int get_frequency(struct file *file, void *priv,
			 struct v4l2_frequency *frequency)
{
	struct ums9117_fm *radio = video_drvdata(file);

	if (frequency->tuner)
		return -EINVAL;
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	frequency->type = V4L2_TUNER_RADIO;
	frequency->frequency = radio->frequency_10khz * UMS9117_FM_FREQ_SCALE;
	return 0;
}

static int set_frequency(struct file *file, void *priv,
			 const struct v4l2_frequency *frequency)
{
	struct ums9117_fm *radio = video_drvdata(file);
	u32 value;
	int ret;

	if (frequency->tuner || frequency->type != V4L2_TUNER_RADIO)
		return -EINVAL;
	value = clamp_t(u32, frequency->frequency,
			UMS9117_FM_MIN_10KHZ * UMS9117_FM_FREQ_SCALE,
			UMS9117_FM_MAX_10KHZ * UMS9117_FM_FREQ_SCALE);
	value = DIV_ROUND_CLOSEST(value, UMS9117_FM_FREQ_SCALE);
	ret = enable(radio);
	if (!ret)
		ret = tune(radio, value);
	return ret;
}

static int seek_frequency(struct file *file, void *priv,
			  const struct v4l2_hw_freq_seek *seek)
{
	struct ums9117_fm *radio = video_drvdata(file);
	u16 previous = radio->frequency_10khz;
	u16 found;
	u8 payload[3];
	u8 reply[9];
	int ret;

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO ||
	    seek->wrap_around || (seek->spacing && seek->spacing != 100000) ||
	    (seek->rangelow &&
	     seek->rangelow != UMS9117_FM_MIN_10KHZ * UMS9117_FM_FREQ_SCALE) ||
	    (seek->rangehigh &&
	     seek->rangehigh != UMS9117_FM_MAX_10KHZ * UMS9117_FM_FREQ_SCALE))
		return -EINVAL;
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	if (radio->state != FM_ON || !previous)
		return -EPIPE;
	/* CM4 includes the start; scans advance after each reported station. */
	put_unaligned_le16(previous, payload);
	payload[2] = !!seek->seek_upward;
	ret = ums9117_hci_fm_command(0x04, payload, sizeof(payload), reply,
				     sizeof(reply), true);
	if (ret < 0)
		return unknown_state(radio, 0x04, ret);
	if (ret == 7) {
		dev_err(radio->v4l2.dev, "FM seek rejected: status=0x%02x\n",
			reply[6]);
		return unknown_state(radio, 0x04, -EREMOTEIO);
	}
	found = get_unaligned_le16(reply + 7);
	if (reply[4] == 1 && !found) {
		ret = tune(radio, previous);
		return ret ? ret : -ENODATA;
	}
	if (reply[4] || found < UMS9117_FM_MIN_10KHZ ||
	    found > UMS9117_FM_MAX_10KHZ) {
		dev_err(radio->v4l2.dev,
			"invalid FM seek result: status=0x%02x frequency=%u (10 kHz)\n",
			reply[4], found);
		return unknown_state(radio, 0x04, -EPROTO);
	}
	/* Do not expose an undocumented firmware wrap as a bounded seek. */
	if ((seek->seek_upward && found < previous) ||
	    (!seek->seek_upward && found > previous)) {
		ret = tune(radio, previous);
		return ret ? ret : -ENODATA;
	}
	radio->frequency_10khz = found;
	return 0;
}

static int set_control(struct v4l2_ctrl *control)
{
	struct ums9117_fm *radio =
		container_of(control->handler, struct ums9117_fm, controls);
	int ret;

	if (radio->state == FM_UNKNOWN)
		return -EIO;
	if (radio->state == FM_OFF)
		return control->val ? 0 : -EPIPE;
	ret = set_byte(radio, 0x02, control->val);
	if (ret)
		dev_err(radio->v4l2.dev, "cannot set FM mute=%d: %pe\n",
			control->val, ERR_PTR(ret));
	return ret;
}

static const struct v4l2_ctrl_ops control_ops = {
	.s_ctrl = set_control,
};

static int radio_open(struct file *file)
{
	struct ums9117_fm *radio = video_drvdata(file);
	int ret;

	mutex_lock(&radio->lock);
	if (!video_is_registered(&radio->video))
		ret = -ENODEV;
	else if (radio->opened)
		ret = -EBUSY;
	else {
		ret = v4l2_fh_open(file);
		if (!ret)
			radio->opened = true;
	}
	mutex_unlock(&radio->lock);
	return ret;
}

static int radio_close(struct file *file)
{
	struct ums9117_fm *radio = video_drvdata(file);

	mutex_lock(&radio->lock);
	if (video_is_registered(&radio->video)) {
		disable(radio);
		if (radio->state == FM_OFF)
			v4l2_ctrl_s_ctrl(radio->mute, 1);
	}
	radio->opened = false;
	v4l2_fh_release(file);
	mutex_unlock(&radio->lock);
	return 0;
}

static const struct v4l2_file_operations radio_fops = {
	.owner = THIS_MODULE,
	.open = radio_open,
	.release = radio_close,
	.unlocked_ioctl = video_ioctl2,
};

static const struct v4l2_ioctl_ops radio_ioctl_ops = {
	.vidioc_querycap = querycap,
	.vidioc_g_tuner = get_tuner,
	.vidioc_s_tuner = set_tuner,
	.vidioc_g_frequency = get_frequency,
	.vidioc_s_frequency = set_frequency,
	.vidioc_s_hw_freq_seek = seek_frequency,
};

static void release_radio(struct v4l2_device *v4l2)
{
	struct ums9117_fm *radio = container_of(v4l2, struct ums9117_fm, v4l2);

	v4l2_ctrl_handler_free(&radio->controls);
	v4l2_device_unregister(&radio->v4l2);
	kfree(radio);
}

struct ums9117_fm *ums9117_fm_register(struct device *dev)
{
	struct ums9117_fm *radio;
	const char *config_name;
	int ret;

	ret = device_property_read_string(dev, "fplinux,fm-config-name",
					  &config_name);
	if (ret)
		return NULL;
	radio = kzalloc(sizeof(*radio), GFP_KERNEL);
	if (!radio)
		return ERR_PTR(-ENOMEM);
	radio->config_name = config_name;
	mutex_init(&radio->lock);
	ret = v4l2_device_register(dev, &radio->v4l2);
	if (ret) {
		kfree(radio);
		return ERR_PTR(ret);
	}
	radio->v4l2.release = release_radio;
	v4l2_ctrl_handler_init(&radio->controls, 1);
	radio->mute = v4l2_ctrl_new_std(&radio->controls, &control_ops,
					V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);
	ret = radio->controls.error;
	if (ret)
		goto put_device;
	strscpy(radio->video.name, "ums9117-fm", sizeof(radio->video.name));
	radio->video.v4l2_dev = &radio->v4l2;
	radio->video.ctrl_handler = &radio->controls;
	radio->video.fops = &radio_fops;
	radio->video.ioctl_ops = &radio_ioctl_ops;
	radio->video.lock = &radio->lock;
	radio->video.release = video_device_release_empty;
	radio->video.device_caps = V4L2_CAP_RADIO | V4L2_CAP_TUNER |
				   V4L2_CAP_HW_FREQ_SEEK;
	video_set_drvdata(&radio->video, radio);
	ret = video_register_device(&radio->video, VFL_TYPE_RADIO, -1);
	if (ret)
		goto put_device;
	return radio;

put_device:
	v4l2_device_put(&radio->v4l2);
	return ERR_PTR(ret);
}

void ums9117_fm_unregister(struct ums9117_fm *radio)
{
	if (!radio)
		return;
	mutex_lock(&radio->lock);
	disable(radio);
	video_unregister_device(&radio->video);
	v4l2_device_disconnect(&radio->v4l2);
	mutex_unlock(&radio->lock);
	v4l2_device_put(&radio->v4l2);
}
