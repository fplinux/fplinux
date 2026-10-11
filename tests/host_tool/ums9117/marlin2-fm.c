// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <media/v4l2-device.h>
#include <linux/completion.h>
#include <linux/unaligned.h>

#include "marlin2-fm.h"
#include "marlin2-transport.h"
#include "marlin2-owner-compat/kernel-boundary.h"

/* Actual FM, transport and framing objects; only external boundaries are fake. */
enum fault {
	NONE,
	SEEK_STATUS,
	SEEK_FORMAT,
	SEEK_TIMEOUT,
};
static struct {
	struct device dev;
	struct mmc_host host;
	struct mmc_card card;
	struct sdio_func func;
	struct gpio_desc gpio;
	struct video_device *video;
	irqreturn_t (*irq)(int, void *);
	void *irq_context;
	u8 response[16];
	size_t response_bytes;
	unsigned int acquires, releases, writes;
	int acquire_error;
	int read_error;
	bool fail_first_irq;
	bool queued;
	enum fault fault;
} bench;

static int acquire(void *context, struct sdio_func **func,
		   struct gpio_desc **request)
{
	assert(context == &bench);
	bench.acquires++;
	if (bench.acquire_error)
		return bench.acquire_error;
	*func = &bench.func;
	*request = &bench.gpio;
	return 0;
}

static void release(void *context)
{
	assert(context == &bench);
	bench.releases++;
}

int request_threaded_irq(int irq, void *primary,
			 irqreturn_t (*thread)(int, void *),
			 unsigned long flags, const char *name, void *context)
{
	assert(irq == 13 && !primary);
	assert(flags == (IRQF_TRIGGER_HIGH | IRQF_ONESHOT));
	(void)name;
	bench.irq = thread;
	bench.irq_context = context;
	if (bench.fail_first_irq) {
		bench.read_error = -EIO;
		assert(thread(13, context) == IRQ_HANDLED);
	}
	return 0;
}

void free_irq(int irq, void *context)
{
	assert(irq == 13 && context == bench.irq_context);
	bench.irq = NULL;
}

void disable_irq_nosync(int irq)
{
	assert(irq == 13);
}

void sdio_claim_host(struct sdio_func *func)
{
	assert(func == &bench.func);
}

void sdio_release_host(struct sdio_func *func)
{
	assert(func == &bench.func);
}

int sdio_readsb(struct sdio_func *func, void *buffer, unsigned int address,
		size_t bytes)
{
	u8 *packet = buffer;

	assert(func == &bench.func && address == 0x20 && bytes == 2048);
	if (bench.read_error)
		return bench.read_error;
	assert(bench.queued);
	memset(packet, 0, bytes);
	put_unaligned_le32(0x10000000U | bench.response_bytes << 7, packet);
	memcpy(packet + 4, bench.response, bench.response_bytes);
	put_unaligned_le32(bench.response_bytes, packet + bytes - 8);
	bench.queued = false;
	return 0;
}

int sdio_writesb(struct sdio_func *func, unsigned int address,
		 const void *buffer, size_t bytes)
{
	static const u8 enable[] = { 1, 0x8c, 0xfc, 3, 0, 0, 1 };
	static const u8 mute[] = { 1, 0x8c, 0xfc, 2, 2, 1 };
	static const u8 tune_low[] = { 1, 0x8c, 0xfc, 3, 1, 0x2e, 0x22 };
	static const u8 tune_play[] = { 1, 0x8c, 0xfc, 3, 1, 0x38, 0x27 };
	static const u8 seek[] = { 1, 0x8c, 0xfc, 4, 4, 0x2e, 0x22, 0 };
	static const u8 disable[] = { 1, 0x8c, 0xfc, 2, 0x12, 0 };
	static const u8 enabled[] = { 4, 0x0e, 4, 1, 0x8c, 0xfc, 0 };
	static const u8 muted[] = { 4, 0x0e, 5, 1, 0x8c, 0xfc, 0, 1 };
	static const u8 tuned_low[] = { 4, 0x0e, 8, 1,	  0x8c, 0xfc,
					0, 0,	 0, 0x2e, 0x22 };
	static const u8 tuned_play[] = { 4, 0x0e, 8, 1,	   0x8c, 0xfc,
					 0, 0,	  0, 0x38, 0x27 };
	static const u8 seek_error[] = { 4, 0x0e, 4,	1,   0x8c, 0xfc,
					 0, 4,	  0xff, 6,   0x30, 2,
					 0, 0,	  0x2e, 0x22 };
	const u8 *packet = buffer;
	const u8 *request = packet + 4;
	const u8 *expected;
	const u8 *response;
	size_t expected_bytes, response_bytes;
	bool low;

	assert(func == &bench.func && address == 0x20 && bytes == 512);
	bench.writes++;
	if (packet[3] == 0x20)
		return 0; /* The other consumer can still transmit during quarantine. */
	assert(packet[3] == 0x10);
	switch (request[4]) {
	case 0:
		expected = enable;
		expected_bytes = sizeof(enable);
		response = enabled;
		response_bytes = sizeof(enabled);
		break;
	case 2:
		expected = mute;
		expected_bytes = sizeof(mute);
		response = muted;
		response_bytes = sizeof(muted);
		break;
	case 1:
		low = request[5] == 0x2e;
		expected = low ? tune_low : tune_play;
		expected_bytes = sizeof(tune_low);
		response = low ? tuned_low : tuned_play;
		response_bytes = sizeof(tuned_low);
		break;
	case 4:
		expected = seek;
		expected_bytes = sizeof(seek);
		response = seek_error;
		response_bytes = sizeof(seek_error);
		break;
	case 0x12:
		expected = disable;
		expected_bytes = sizeof(disable);
		response = enabled;
		response_bytes = sizeof(enabled);
		break;
	default:
		assert(false);
		return -EINVAL;
	}
	assert(!memcmp(request, expected, expected_bytes));
	assert(((get_unaligned_le32(packet) >> 7) & 0xffff) == expected_bytes);
	memcpy(bench.response, response, response_bytes);
	bench.response_bytes = response_bytes;
	bench.queued = request[4] != 4 || bench.fault != SEEK_TIMEOUT;
	if (request[4] == 4 && bench.fault == SEEK_FORMAT)
		bench.response[0] = 3;
	return 0;
}

unsigned long wait_for_completion_timeout(struct completion *done,
					  unsigned long timeout)
{
	assert(timeout == 4000 || timeout == 20000);
	if (bench.queued)
		assert(bench.irq(13, bench.irq_context) == IRQ_HANDLED);
	return done->done ? 1 : 0;
}

/* Reuse the existing V4L2 registration facade; no V4L2 core is executed. */
int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2)
{
	v4l2->dev = dev;
	return 0;
}
void v4l2_device_unregister(struct v4l2_device *v4l2)
{
	(void)v4l2;
}

void v4l2_device_disconnect(struct v4l2_device *v4l2)
{
	(void)v4l2;
}

void v4l2_device_put(struct v4l2_device *v4l2)
{
	v4l2->release(v4l2);
}

int video_register_device(struct video_device *video, int type, int number)
{
	assert(type == 2 && number == -1);
	video->registered = true;
	bench.video = video;
	return 0;
}
void video_unregister_device(struct video_device *video)
{
	video->registered = false;
}

void video_device_release_empty(struct video_device *video)
{
	(void)video;
}

int v4l2_fh_open(struct file *file)
{
	(void)file;
	return 0;
}

int v4l2_fh_release(struct file *file)
{
	(void)file;
	return 0;
}

long video_ioctl2(struct file *file, unsigned int request, unsigned long arg)
{
	(void)file;
	(void)request;
	(void)arg;
	abort();
}
void v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *handler,
			    unsigned int count)
{
	assert(count == 1);
	handler->error = 0;
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

	assert(control && id == 0x00980909 && min == 0 && max == 1);
	assert(step == 1 && value == 1);
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

static int tune(struct file *file, bool play)
{
	const struct v4l2_frequency frequency = {
		.type = 1,
		.frequency = play ? 1606400 : 1400000,
	};

	return file->video->ioctl_ops->vidioc_s_frequency(file, NULL,
							  &frequency);
}

static void check_frequency(struct file *file, int error, u32 value)
{
	struct v4l2_frequency frequency = {};
	int ret = file->video->ioctl_ops->vidioc_g_frequency(file, NULL,
							     &frequency);

	assert(ret == error);
	if (!ret)
		assert(frequency.frequency == value);
}

static void unused_receive(void *context, u8 subtype, const u8 *data,
			   size_t bytes)
{
	(void)context;
	(void)subtype;
	(void)data;
	(void)bytes;
}

static void check_recovery(enum fault fault, int error, bool with_other,
			   bool failed_boot, bool failed_irq)
{
	const struct marlin2_transport_radio_ops radio_ops = {
		.acquire = acquire,
		.release = release,
	};
	const struct marlin2_channel_ops other_ops = { .receive =
							       unused_receive };
	const struct v4l2_hw_freq_seek seek = { .type = 1, .seek_upward = 1 };
	struct marlin2_transport *transport;
	struct marlin2_fm *radio;
	struct marlin2_channel *other = NULL;
	struct file file;
	unsigned int writes;
	int ret;

	memset(&bench, 0, sizeof(bench));
	bench.host = (struct mmc_host){ 10240, 10240, 20 };
	bench.card.host = &bench.host;
	bench.card.cccr.multi_block = true;
	bench.func = (struct sdio_func){ &bench.card, 1, 512 };
	bench.gpio.irq = 13;
	transport = marlin2_transport_create(&bench.dev, &radio_ops, &bench);
	assert(!IS_ERR(transport));
	radio = marlin2_fm_register(&bench.dev, transport);
	assert(!IS_ERR(radio) && bench.video);
	file = (struct file){ .video = bench.video };
	assert(!file.video->fops->open(&file));
	assert(!tune(&file, false));
	if (with_other) {
		other = marlin2_channel_register(transport, MARLIN2_CLIENT_WLAN,
						 &other_ops, NULL);
		assert(!IS_ERR(other) && !marlin2_channel_open(other));
	}
	bench.fault = fault;
	assert(file.video->ioctl_ops->vidioc_s_hw_freq_seek(&file, NULL,
							    &seek) == error);
	check_frequency(&file, -EIO, 0);
	writes = bench.writes;
	assert(tune(&file, true) == -EIO && bench.writes == writes);
	assert(!file.video->fops->release(&file));
	assert(bench.releases == (with_other ? 0U : 1U));
	assert(!file.video->fops->open(&file));
	if (with_other) {
		static const u8 payload[] = { 0x55 };
		assert(tune(&file, true) == -EBUSY);
		assert(bench.acquires == 1 && !bench.releases);
		check_frequency(&file, -EIO, 0);
		assert(!marlin2_channel_send(other, 0, payload,
					     sizeof(payload)));
		marlin2_channel_close(other);
		assert(bench.releases == 1);
	}
	if (failed_boot) {
		bench.acquire_error = -EHOSTDOWN;
		assert(tune(&file, true) == -EHOSTDOWN);
		check_frequency(&file, -EIO, 0);
		assert(bench.writes == writes && bench.releases == 1);
		bench.acquire_error = 0;
	}
	bench.fault = NONE;
	bench.fail_first_irq = failed_irq;
	ret = tune(&file, true);
	printf("reopen tune: fault=%d other=%d failed_boot=%d failed_irq=%d result=%d\n",
	       fault, with_other, failed_boot, failed_irq, ret);
	fflush(stdout);
	assert(ret == (failed_irq ? -EIO : 0));
	if (failed_irq) {
		check_frequency(&file, -EIO, 0);
		assert(bench.writes == writes);
	} else {
		check_frequency(&file, 0, 1606400);
	}
	assert(bench.acquires == (failed_boot ? 3U : 2U));
	assert(!file.video->fops->release(&file));
	assert(bench.releases == 2);
	marlin2_fm_unregister(radio);
	if (other)
		marlin2_channel_unregister(other);
	marlin2_transport_destroy(transport);
}

int main(void)
{
	check_recovery(SEEK_STATUS, -EREMOTEIO, false, false, false);
	check_recovery(SEEK_FORMAT, -EPROTO, false, false, false);
	check_recovery(SEEK_TIMEOUT, -ETIMEDOUT, false, false, false);
	check_recovery(SEEK_STATUS, -EREMOTEIO, true, false, false);
	check_recovery(SEEK_STATUS, -EREMOTEIO, false, true, false);
	check_recovery(SEEK_STATUS, -EREMOTEIO, false, false, true);
	puts("Marlin2 FM fresh runtime host consumer cases passed");
	return 0;
}
