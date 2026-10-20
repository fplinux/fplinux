/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_CAPTURE_FIFO_H
#define FPLINUX_QIN_CAPTURE_FIFO_H

#include <linux/errno.h>

static inline int qin_pcm_ring_used(unsigned long producer,
				    unsigned long consumer,
				    unsigned long boundary,
				    unsigned long buffer_frames,
				    unsigned long *used_frames)
{
	unsigned long unread;

	if (!boundary || producer >= boundary || consumer >= boundary)
		return -EPIPE;
	if (producer >= consumer)
		unread = producer - consumer;
	else
		unread = boundary - consumer + producer;
	if (unread > buffer_frames)
		return -EPIPE;
	*used_frames = unread;
	return 0;
}

static inline int qin_capture_ring_free(unsigned long producer,
					unsigned long application,
					unsigned long boundary,
					unsigned long buffer_frames,
					unsigned long *free_frames)
{
	unsigned long used;
	int ret;

	ret = qin_pcm_ring_used(producer, application, boundary, buffer_frames,
				&used);
	if (!ret)
		*free_frames = buffer_frames - used;
	return ret;
}

#endif
