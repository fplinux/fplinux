// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <linux/firmware.h>
#include <linux/property.h>
#include <linux/unaligned.h>
#include <media/v4l2-device.h>

#include "cm4-fm.h"
#include "cm4-hci.h"

/* External firmware and V4L2 registration doubles; the driver is linked whole. */
enum response_fault {
	REPLY_OK,
	READ_ERROR,
	READ_ADDRESS,
	READ_OPERATION,
	READ_SHORT,
	WRITE_ERROR,
	WRITE_READBACK,
	MUTE_TIMEOUT,
	TUNE_ECHO,
};

static struct {
	struct video_device *video;
	u32 clock;
	unsigned int writes;
	bool enabled;
	bool muted;
	bool held;
	int quarantine;
	enum response_fault fault;
} fake;

int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	*value = "test-fm.bin";
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name,
		     struct device *dev)
{
	static const u8 config[128];
	static const struct firmware firmware = { sizeof(config), config };

	*fw = &firmware;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
}

int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2)
{
	v4l2->dev = dev;
	return 0;
}

void v4l2_device_unregister(struct v4l2_device *v4l2)
{
}

void v4l2_device_disconnect(struct v4l2_device *v4l2)
{
}

void v4l2_device_put(struct v4l2_device *v4l2)
{
	v4l2->release(v4l2);
}

int video_register_device(struct video_device *video, int type, int number)
{
	video->registered = true;
	fake.video = video;
	return 0;
}

void video_unregister_device(struct video_device *video)
{
	video->registered = false;
}

void video_device_release_empty(struct video_device *video)
{
}

int v4l2_fh_open(struct file *file)
{
	return 0;
}

int v4l2_fh_release(struct file *file)
{
	return 0;
}

long video_ioctl2(struct file *file, unsigned int request, unsigned long arg)
{
	abort();
}

void v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *handler,
			    unsigned int count)
{
	assert(count == 1);
}

void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *handler)
{
	free(handler->control);
}

struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *handler,
				    const struct v4l2_ctrl_ops *ops, u32 id,
				    int min, int max, int step, int value)
{
	struct v4l2_ctrl *control = calloc(1, sizeof(*control));

	assert(control);
	control->handler = handler;
	control->ops = ops;
	control->val = value;
	handler->control = control;
	return control;
}

int v4l2_ctrl_s_ctrl(struct v4l2_ctrl *control, int value)
{
	control->val = value;
	return control->ops->s_ctrl(control);
}

int ums9117_hci_fm_hold(void)
{
	assert(!fake.held);
	fake.held = true;
	return 0;
}

void ums9117_hci_fm_release(void)
{
	assert(fake.held && !fake.enabled);
	assert(fake.clock == 0xa47b);
	fake.held = false;
}

void ums9117_hci_fm_quarantine(int error)
{
	assert(error < 0);
	fake.quarantine = error;
}

static int clock_reply(const u8 *payload, size_t bytes, u8 *reply,
		       size_t capacity)
{
	static const u8 read_request[] = { 0, 0x94, 0x41, 0, 0, 0, 0, 0, 0, 1 };
	static const u8 write_request[] = { 0,	  0x94, 0x41, 0, 0,
					    0x7b, 0xa4, 0,    0, 0 };
	static const u8 header[] = { 4, 0x0e, 0x0e, 1, 0x8c, 0xfc, 0 };
	bool read;

	assert(fake.enabled && fake.muted && fake.held);
	assert(bytes == 10 && capacity == 17);
	read = payload[9] == 1;
	assert(!memcmp(payload, read ? read_request : write_request, 10));
	if (!read) {
		fake.writes++;
		fake.clock = 0xa47b;
	}
	memcpy(reply, header, sizeof(header));
	reply[7] = 0;
	put_unaligned_le32(0x4194, reply + 8);
	put_unaligned_le32(fake.clock, reply + 12);
	reply[16] = read ? 3 : 2;
	if ((read && fake.fault == READ_ERROR) ||
	    (!read && fake.fault == WRITE_ERROR))
		reply[7] = 1;
	if (read && fake.fault == READ_ADDRESS)
		reply[8] = 0x95;
	if (read && fake.fault == READ_OPERATION)
		reply[16] = 1;
	if (!read && fake.fault == WRITE_READBACK)
		reply[13] = 0xa0;
	return read && fake.fault == READ_SHORT ? 16 : 17;
}

int ums9117_hci_fm_command(u8 subcommand, const u8 *payload,
			   size_t payload_bytes, u8 *reply, size_t reply_bytes,
			   bool seek)
{
	assert(fake.held && !fake.quarantine && !seek);
	memset(reply, 0, reply_bytes);
	switch (subcommand) {
	case 0x00:
		fake.enabled = true;
		break;
	case 0x01:
		assert(payload_bytes == 2 && reply_bytes == 11);
		memcpy(reply + 9, payload, 2);
		if (fake.fault == TUNE_ECHO)
			reply[9]++;
		break;
	case 0x02:
		if (fake.fault == MUTE_TIMEOUT)
			return -ETIMEDOUT;
		fake.muted = !!payload[0];
		reply[7] = payload[0];
		break;
	case 0x1b:
		reply[7] = payload[0];
		break;
	case 0x22:
		return clock_reply(payload, payload_bytes, reply, reply_bytes);
	case 0x12:
		assert(fake.clock == 0xa47b && fake.muted);
		fake.enabled = false;
		break;
	default:
		abort();
	}
	return reply_bytes;
}

static int tune(struct file *file)
{
	const struct v4l2_frequency frequency = {
		.type = V4L2_TUNER_RADIO,
		.frequency = 1628800,
	};

	return file->video->ioctl_ops->vidioc_s_frequency(file, NULL,
							  &frequency);
}

static struct ums9117_fm *open_radio(struct file *file, u32 clock)
{
	static struct device device;
	struct ums9117_fm *radio;

	memset(&fake, 0, sizeof(fake));
	fake.clock = clock;
	radio = ums9117_fm_register(&device);
	assert(radio && fake.video);
	*file = (struct file){ .video = fake.video };
	assert(file->video->fops->open(file) == 0);
	return radio;
}

static void normal_close_restores_clock_and_releases_hold(void)
{
	struct file file;
	struct ums9117_fm *radio = open_radio(&file, 0xa57b);

	assert(tune(&file) == 0);
	assert(fake.held && fake.enabled);
	assert(file.video->fops->release(&file) == 0);
	assert(fake.clock == 0xa47b && fake.writes == 1);
	assert(!fake.enabled && !fake.held && !fake.quarantine);
	ums9117_fm_unregister(radio);
}

static void clear_clock_closes_without_rf_write(void)
{
	struct file file;
	struct ums9117_fm *radio = open_radio(&file, 0xa47b);

	assert(tune(&file) == 0);
	assert(file.video->fops->release(&file) == 0);
	assert(fake.clock == 0xa47b && !fake.writes);
	assert(!fake.enabled && !fake.held && !fake.quarantine);
	ums9117_fm_unregister(radio);
}

static void failed_close_keeps_hold_and_rejects_reopen_tune(void)
{
	static const struct {
		enum response_fault fault;
		int error;
	} cases[] = {
		{ READ_ERROR, -EREMOTEIO },   { READ_ADDRESS, -EPROTO },
		{ READ_OPERATION, -EPROTO },  { READ_SHORT, -EPROTO },
		{ WRITE_ERROR, -EREMOTEIO },  { WRITE_READBACK, -EPROTO },
		{ MUTE_TIMEOUT, -ETIMEDOUT },
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		struct file file;
		struct ums9117_fm *radio = open_radio(&file, 0xa57b);

		assert(tune(&file) == 0);
		fake.fault = cases[index].fault;
		assert(file.video->fops->release(&file) == 0);
		assert(fake.held && fake.enabled);
		assert(fake.quarantine == cases[index].error);
		assert(file.video->fops->open(&file) == 0);
		assert(tune(&file) == -EIO);
		assert(file.video->fops->release(&file) == 0);
		ums9117_fm_unregister(radio);
		assert(fake.held);
	}
}

static void failed_tune_stays_quarantined_on_close(void)
{
	struct file file;
	struct ums9117_fm *radio = open_radio(&file, 0xa57b);

	fake.fault = TUNE_ECHO;
	assert(tune(&file) == -EPROTO);
	assert(file.video->fops->release(&file) == 0);
	assert(fake.held && fake.enabled && !fake.writes);
	assert(fake.quarantine == -EPROTO);
	ums9117_fm_unregister(radio);
}

int main(void)
{
	normal_close_restores_clock_and_releases_hold();
	clear_clock_closes_without_rf_write();
	failed_close_keeps_hold_and_rejects_reopen_tune();
	failed_tune_stays_quarantined_on_close();
	return 0;
}
