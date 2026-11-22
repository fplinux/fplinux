// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/soc/ums9117/fm-wire.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "marlin2-fm.h"
#include "marlin2-transport.h"

#define MARLIN2_FM_MIN_10KHZ 8750
#define MARLIN2_FM_MAX_10KHZ 10800
#define MARLIN2_FM_FREQ_SCALE 160
#define MARLIN2_FM_COMMAND_TIMEOUT_MS 4000
#define MARLIN2_FM_SEEK_TIMEOUT_MS 20000

enum fm_state {
	FM_OFF,
	FM_ON,
	FM_UNKNOWN,
};

struct marlin2_fm {
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *mute;
	struct mutex lock;
	spinlock_t rx_lock;
	struct completion done;
	struct ums9117_fm_wire wire;
	u8 reply[11];
	int error;
	bool pending;
	struct marlin2_channel *channel;
	enum fm_state state;
	u16 frequency_10khz;
	bool opened;
	bool held;
};

static int unknown_state(struct marlin2_fm *radio, int error)
{
	radio->state = FM_UNKNOWN;
	return error;
}

static void transport_error(void *context, int error)
{
	struct marlin2_fm *radio = context;
	unsigned long flags;

	spin_lock_irqsave(&radio->rx_lock, flags);
	radio->error = error;
	radio->pending = false;
	radio->wire.event_bytes = 0;
	complete(&radio->done);
	spin_unlock_irqrestore(&radio->rx_lock, flags);
}

static void receive(void *context, u8 subtype, const u8 *payload,
		    size_t payload_bytes)
{
	struct marlin2_fm *radio = context;
	unsigned long flags;
	bool pending;
	int ret;

	if (subtype)
		return;
	spin_lock_irqsave(&radio->rx_lock, flags);
	pending = radio->pending;
	ret = radio->error;
	if (!ret)
		ret = ums9117_fm_wire_receive(&radio->wire, pending, payload,
					      payload_bytes);
	if (ret) {
		radio->error = ret;
		radio->pending = false;
		radio->wire.event_bytes = 0;
	} else if (radio->wire.complete) {
		radio->pending = false;
	}
	if (ret || (pending && !radio->pending))
		complete(&radio->done);
	spin_unlock_irqrestore(&radio->rx_lock, flags);
}

/* The video lock serializes requests; RX owns only rx_lock. */
static int command(struct marlin2_fm *radio, u8 subcommand, const u8 *payload,
		   size_t payload_bytes, u8 *reply, size_t reply_bytes,
		   bool seek)
{
	u8 request[UMS9117_FM_COMMAND_BYTES];
	unsigned long flags;
	size_t request_bytes;
	size_t received_bytes = 0;
	unsigned int timeout_ms = seek ? MARLIN2_FM_SEEK_TIMEOUT_MS :
					 MARLIN2_FM_COMMAND_TIMEOUT_MS;
	int ret;

	request_bytes = ums9117_fm_command_build(
		request, sizeof(request), subcommand, payload, payload_bytes);
	if (!request_bytes)
		return -EINVAL;
	spin_lock_irqsave(&radio->rx_lock, flags);
	reinit_completion(&radio->done);
	if (radio->error)
		ret = radio->error;
	else if (radio->pending || radio->wire.event_bytes)
		ret = -EBUSY;
	else
		ret = ums9117_fm_wire_begin(&radio->wire, radio->reply,
					    sizeof(radio->reply), reply_bytes,
					    seek);
	if (!ret)
		radio->pending = true;
	spin_unlock_irqrestore(&radio->rx_lock, flags);
	if (ret)
		return unknown_state(radio, ret);
	ret = marlin2_channel_send(radio->channel, 0, request, request_bytes);
	if (ret) {
		transport_error(radio, ret);
		return unknown_state(radio, ret);
	}
	if (!wait_for_completion_timeout(&radio->done,
					 msecs_to_jiffies(timeout_ms))) {
		transport_error(radio, -ETIMEDOUT);
		return unknown_state(radio, -ETIMEDOUT);
	}
	spin_lock_irqsave(&radio->rx_lock, flags);
	ret = radio->error;
	if (!ret) {
		received_bytes = radio->wire.reply_bytes;
		memcpy(reply, radio->reply, received_bytes);
	}
	spin_unlock_irqrestore(&radio->rx_lock, flags);
	if (ret < 0)
		return unknown_state(radio, ret);
	if (reply[1] == 0x0e && reply[6])
		return unknown_state(radio, -EREMOTEIO);
	if (received_bytes != reply_bytes)
		return unknown_state(radio, -EPROTO);
	return 0;
}

static int set_mute(struct marlin2_fm *radio, bool muted)
{
	u8 value = muted;
	u8 reply[8];
	int ret;

	ret = command(radio, 0x02, &value, 1, reply, sizeof(reply), false);
	if (!ret && reply[7] != value)
		ret = unknown_state(radio, -EPROTO);
	return ret;
}

static int enable(struct marlin2_fm *radio)
{
	const u8 payload[2] = { 0x00, 0x01 };
	u8 reply[7];
	unsigned long flags;
	int ret;

	if (radio->state == FM_UNKNOWN && radio->held)
		return -EIO;
	if (radio->state == FM_ON)
		return 0;
	if (radio->state == FM_UNKNOWN) {
		ret = marlin2_channel_open_fresh(radio->channel);
		if (ret)
			return ret;
		/* The prior runtime is gone; RX still owns its transaction lock. */
		spin_lock_irqsave(&radio->rx_lock, flags);
		memset(&radio->wire, 0, sizeof(radio->wire));
		radio->error = 0;
		radio->pending = false;
		reinit_completion(&radio->done);
		spin_unlock_irqrestore(&radio->rx_lock, flags);
		radio->state = FM_OFF;
		radio->frequency_10khz = 0;
	} else {
		ret = marlin2_channel_open(radio->channel);
	}
	if (ret)
		return ret;
	radio->held = true;
	/* Enabling starts muted, including after a fresh runtime recovery. */
	ret = v4l2_ctrl_s_ctrl(radio->mute, 1);
	if (ret)
		return ret;
	ret = command(radio, 0x00, payload, sizeof(payload), reply,
		      sizeof(reply), false);
	if (ret)
		return ret;
	radio->state = FM_ON;
	return set_mute(radio, true);
}

static int disable(struct marlin2_fm *radio)
{
	u8 value = 0;
	u8 reply[7];
	int ret;

	if (radio->state == FM_OFF)
		return 0;
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	ret = set_mute(radio, true);
	if (!ret)
		ret = command(radio, 0x12, &value, 1, reply, sizeof(reply),
			      false);
	if (!ret) {
		radio->state = FM_OFF;
		radio->frequency_10khz = 0;
	}
	return ret;
}

static int tune(struct marlin2_fm *radio, u16 frequency_10khz)
{
	u8 payload[2];
	u8 reply[11];
	int ret;

	put_unaligned_le16(frequency_10khz, payload);
	ret = command(radio, 0x01, payload, sizeof(payload), reply,
		      sizeof(reply), false);
	if (!ret && get_unaligned_le16(reply + 9) != frequency_10khz)
		ret = unknown_state(radio, -EPROTO);
	if (!ret)
		radio->frequency_10khz = frequency_10khz;
	return ret;
}

static int querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "marlin2-fm", sizeof(cap->driver));
	strscpy(cap->card, "Marlin2 FM receiver", sizeof(cap->card));
	strscpy(cap->bus_info, "sdio:marlin2", sizeof(cap->bus_info));
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
	tuner->rangelow = MARLIN2_FM_MIN_10KHZ * MARLIN2_FM_FREQ_SCALE;
	tuner->rangehigh = MARLIN2_FM_MAX_10KHZ * MARLIN2_FM_FREQ_SCALE;
	tuner->audmode = V4L2_TUNER_MODE_STEREO;
	tuner->signal = 0;
	tuner->rxsubchans = 0;
	tuner->afc = 0;
	return 0;
}

static int set_tuner(struct file *file, void *priv,
		     const struct v4l2_tuner *tuner)
{
	return tuner->index ? -EINVAL : 0;
}

static int get_frequency(struct file *file, void *priv,
			 struct v4l2_frequency *frequency)
{
	struct marlin2_fm *radio = video_drvdata(file);

	if (frequency->tuner)
		return -EINVAL;
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	frequency->type = V4L2_TUNER_RADIO;
	frequency->frequency = radio->frequency_10khz * MARLIN2_FM_FREQ_SCALE;
	return 0;
}

static int set_frequency(struct file *file, void *priv,
			 const struct v4l2_frequency *frequency)
{
	struct marlin2_fm *radio = video_drvdata(file);
	u32 value;
	int ret;

	if (frequency->tuner || frequency->type != V4L2_TUNER_RADIO)
		return -EINVAL;
	value = clamp_t(u32, frequency->frequency,
			MARLIN2_FM_MIN_10KHZ * MARLIN2_FM_FREQ_SCALE,
			MARLIN2_FM_MAX_10KHZ * MARLIN2_FM_FREQ_SCALE);
	value = DIV_ROUND_CLOSEST(value, MARLIN2_FM_FREQ_SCALE);
	ret = enable(radio);
	if (!ret)
		ret = tune(radio, value);
	return ret;
}

static int seek_frequency(struct file *file, void *priv,
			  const struct v4l2_hw_freq_seek *seek)
{
	struct marlin2_fm *radio = video_drvdata(file);
	u16 previous = radio->frequency_10khz;
	u16 found;
	u8 payload[3];
	u8 reply[9];
	int ret;

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO ||
	    seek->wrap_around || (seek->spacing && seek->spacing != 100000) ||
	    (seek->rangelow &&
	     seek->rangelow != MARLIN2_FM_MIN_10KHZ * MARLIN2_FM_FREQ_SCALE) ||
	    (seek->rangehigh &&
	     seek->rangehigh != MARLIN2_FM_MAX_10KHZ * MARLIN2_FM_FREQ_SCALE))
		return -EINVAL;
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	if (radio->state == FM_UNKNOWN)
		return -EIO;
	if (radio->state != FM_ON || !previous)
		return -EPIPE;
	put_unaligned_le16(previous, payload);
	payload[2] = !seek->seek_upward;
	ret = command(radio, 0x04, payload, sizeof(payload), reply,
		      sizeof(reply), true);
	if (ret)
		return ret;
	if (reply[4])
		return unknown_state(radio, -EREMOTEIO);
	found = get_unaligned_le16(reply + 7);
	if (found < MARLIN2_FM_MIN_10KHZ || found > MARLIN2_FM_MAX_10KHZ)
		return unknown_state(radio, -EPROTO);
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
	struct marlin2_fm *radio =
		container_of(control->handler, struct marlin2_fm, controls);

	if (radio->state == FM_UNKNOWN)
		return -EIO;
	if (radio->state == FM_OFF)
		return control->val ? 0 : -EPIPE;
	return set_mute(radio, control->val);
}

static const struct v4l2_ctrl_ops control_ops = {
	.s_ctrl = set_control,
};

static int radio_open(struct file *file)
{
	struct marlin2_fm *radio = video_drvdata(file);
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
	struct marlin2_fm *radio = video_drvdata(file);

	mutex_lock(&radio->lock);
	if (video_is_registered(&radio->video))
		disable(radio);
	if (radio->held) {
		marlin2_channel_close(radio->channel);
		radio->held = false;
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
	struct marlin2_fm *radio = container_of(v4l2, struct marlin2_fm, v4l2);

	v4l2_ctrl_handler_free(&radio->controls);
	v4l2_device_unregister(&radio->v4l2);
	kfree(radio);
}

struct marlin2_fm *marlin2_fm_register(struct device *dev,
				       struct marlin2_transport *transport)
{
	static const struct marlin2_channel_ops channel_ops = {
		.receive = receive,
		.error = transport_error,
	};
	struct marlin2_fm *radio;
	int ret;

	radio = kzalloc_obj(*radio, GFP_KERNEL);
	if (!radio)
		return ERR_PTR(-ENOMEM);
	mutex_init(&radio->lock);
	spin_lock_init(&radio->rx_lock);
	init_completion(&radio->done);
	ret = v4l2_device_register(dev, &radio->v4l2);
	if (ret) {
		kfree(radio);
		return ERR_PTR(ret);
	}
	radio->v4l2.release = release_radio;
	v4l2_ctrl_handler_init(&radio->controls, 1);
	radio->channel = marlin2_channel_register(transport, MARLIN2_CLIENT_FM,
						  &channel_ops, radio);
	if (IS_ERR(radio->channel)) {
		ret = PTR_ERR(radio->channel);
		radio->channel = NULL;
		goto put_device;
	}
	radio->mute = v4l2_ctrl_new_std(&radio->controls, &control_ops,
					V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);
	ret = radio->controls.error;
	if (ret)
		goto put_device;
	strscpy(radio->video.name, "marlin2-fm", sizeof(radio->video.name));
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
	if (radio->channel)
		marlin2_channel_unregister(radio->channel);
	v4l2_device_put(&radio->v4l2);
	return ERR_PTR(ret);
}

void marlin2_fm_unregister(struct marlin2_fm *radio)
{
	if (!radio)
		return;
	mutex_lock(&radio->lock);
	disable(radio);
	if (radio->held) {
		marlin2_channel_close(radio->channel);
		radio->held = false;
	}
	marlin2_channel_unregister(radio->channel);
	radio->channel = NULL;
	video_unregister_device(&radio->video);
	v4l2_device_disconnect(&radio->v4l2);
	mutex_unlock(&radio->lock);
	v4l2_device_put(&radio->v4l2);
}
