/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_PLAYBACK_H
#define FPLINUX_QIN_PLAYBACK_H

#include <linux/errno.h>
#include <linux/soc/sprd/vbc-fifo.h>
#include <linux/types.h>

#include "qin-capture-fifo.h"

enum qin_playback_result {
	QIN_PLAYBACK_IDLE,
	QIN_PLAYBACK_PERIOD,
	QIN_PLAYBACK_DRAINED,
};

struct qin_playback {
	unsigned long submit_ptr;
	unsigned long hw_pos;
	unsigned long period_frames;
	u64 submitted_frames;
	u64 consumed_frames;
};

struct qin_playback_buffer {
	const u8 *samples;
	unsigned long application;
	unsigned long boundary;
	unsigned long frames;
	unsigned long period;
	bool draining;
};

struct qin_playback_step {
	unsigned long advanced;
	unsigned long written;
	const char *fault;
};

typedef void (*qin_playback_write_frame)(void *context, u32 left, u32 right);

static inline int qin_playback_queued(u32 status0, u32 status1)
{
	int queued0 = sprd_vbc_fifo_frames(status0);
	int queued1 = sprd_vbc_fifo_frames(status1);

	if (queued0 < 0 || queued1 < 0)
		return -EIO;
	/* The two status reads can straddle one sample consumption edge. */
	return queued0 > queued1 ? queued0 : queued1;
}

static inline int
qin_playback_check_prefill(const struct qin_playback *playback, u32 status0,
			   u32 status1)
{
	int queued0 = sprd_vbc_fifo_frames(status0);
	int queued1 = sprd_vbc_fifo_frames(status1);

	/* The consumer is stopped, so both lanes must contain every written pair. */
	if (queued0 < 0 || queued1 < 0 ||
	    (u64)queued0 != playback->submitted_frames ||
	    (u64)queued1 != playback->submitted_frames)
		return -EIO;
	return 0;
}

static inline void qin_playback_fill(struct qin_playback *playback,
				     const struct qin_playback_buffer *buffer,
				     unsigned long frames,
				     qin_playback_write_frame write_frame,
				     void *context)
{
	unsigned long i;

	for (i = 0; i < frames; i++) {
		unsigned long sample =
			(playback->submit_ptr % buffer->frames) * 4;
		u16 left = buffer->samples[sample] |
			   (u16)buffer->samples[sample + 1] << 8;
		u16 right = buffer->samples[sample + 2] |
			    (u16)buffer->samples[sample + 3] << 8;

		write_frame(context, (u32)(s32)(s16)left, (u32)(s32)(s16)right);
		if (++playback->submit_ptr == buffer->boundary)
			playback->submit_ptr = 0;
	}
	playback->submitted_frames += frames;
}

static inline int qin_playback_prime(struct qin_playback *playback,
				     const struct qin_playback_buffer *buffer,
				     u32 status0, u32 status1,
				     qin_playback_write_frame write_frame,
				     void *context)
{
	unsigned long pending;
	int ret;

	if (!buffer->frames || !buffer->period)
		return -EINVAL;
	ret = qin_playback_queued(status0, status1);
	if (ret)
		return ret < 0 ? ret : -EIO;
	ret = qin_pcm_ring_used(buffer->application, playback->submit_ptr,
				buffer->boundary, buffer->frames, &pending);
	if (ret)
		return ret;
	if (!pending)
		return -EPIPE;
	if (pending > SPRD_VBC_FIFO_FRAMES)
		pending = SPRD_VBC_FIFO_FRAMES;
	qin_playback_fill(playback, buffer, pending, write_frame, context);
	return 0;
}

/* Only consumed samples advance the ALSA hardware pointer and periods. */
static inline int qin_playback_service(struct qin_playback *playback,
				       const struct qin_playback_buffer *buffer,
				       u32 status0, u32 status1,
				       qin_playback_write_frame write_frame,
				       void *context,
				       struct qin_playback_step *step)
{
	unsigned long pending;
	u64 consumed;
	int queued;
	int ret;

	*step = (struct qin_playback_step){};
	queued = qin_playback_queued(status0, status1);
	if (queued < 0 || !buffer->frames || !buffer->period ||
	    playback->consumed_frames > playback->submitted_frames ||
	    (u64)queued >
		    playback->submitted_frames - playback->consumed_frames) {
		step->fault = "FIFO state invalid";
		return -EIO;
	}
	consumed = playback->submitted_frames - queued;
	step->advanced = consumed - playback->consumed_frames;
	playback->consumed_frames = consumed;
	playback->hw_pos = (playback->hw_pos + step->advanced) % buffer->frames;
	playback->period_frames += step->advanced;
	ret = qin_pcm_ring_used(buffer->application, playback->submit_ptr,
				buffer->boundary, buffer->frames, &pending);
	if (ret) {
		step->fault = "application pointer invalid";
		return ret;
	}
	if (!queued) {
		if (!pending && buffer->draining)
			return QIN_PLAYBACK_DRAINED;
		step->fault = "FIFO underrun";
		return -EPIPE;
	}
	step->written = SPRD_VBC_FIFO_FRAMES - queued;
	if (step->written > pending)
		step->written = pending;
	qin_playback_fill(playback, buffer, step->written, write_frame,
			  context);
	if (playback->period_frames >= buffer->period) {
		playback->period_frames %= buffer->period;
		return QIN_PLAYBACK_PERIOD;
	}
	return QIN_PLAYBACK_IDLE;
}

#endif
