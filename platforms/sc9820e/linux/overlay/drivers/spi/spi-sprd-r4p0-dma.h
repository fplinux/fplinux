/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPRD_SPI_R4P0_DMA_H
#define FPLINUX_SPRD_SPI_R4P0_DMA_H

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/dma/sprd-dma.h>
#include <linux/dmaengine.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SPRD_SPI_R4P0_DMA_BUFFER_BYTES SZ_64K
#define SPRD_SPI_R4P0_DMA_FRAGMENT_BYTES 8

enum sprd_spi_r4p0_dma_state {
	SPRD_SPI_R4P0_DMA_IDLE,
	SPRD_SPI_R4P0_DMA_ACTIVE,
	SPRD_SPI_R4P0_DMA_QUARANTINED,
};

struct sprd_spi_r4p0_dma {
	struct dma_chan *channel;
	struct device *dma_dev;
	void *buffer;
	dma_addr_t address;
	phys_addr_t fifo;
	struct completion done;
	int result;
	enum sprd_spi_r4p0_dma_state state;
};

static inline bool sprd_spi_r4p0_dma_quarantined(struct sprd_spi_r4p0_dma *dma)
{
	return dma && dma->state == SPRD_SPI_R4P0_DMA_QUARANTINED;
}

static inline void
sprd_spi_r4p0_dma_complete(void *data, const struct dmaengine_result *result)
{
	struct sprd_spi_r4p0_dma *dma = data;

	dma->result = result && result->result == DMA_TRANS_NOERROR ? 0 : -EIO;
	complete(&dma->done);
}

static inline struct sprd_spi_r4p0_dma *
sprd_spi_r4p0_dma_create(struct device *dev, phys_addr_t fifo)
{
	struct sprd_spi_r4p0_dma *dma;
	int ret;

	dma = kzalloc_obj(*dma);
	if (!dma)
		return ERR_PTR(-ENOMEM);

	dma->channel = dma_request_chan(dev, "tx_chn");
	if (IS_ERR(dma->channel)) {
		ret = PTR_ERR(dma->channel);
		goto free_context;
	}
	dma->dma_dev = dmaengine_get_dma_device(dma->channel);
	dma->buffer = dma_alloc_coherent(dma->dma_dev,
					 SPRD_SPI_R4P0_DMA_BUFFER_BYTES,
					 &dma->address, GFP_KERNEL);
	if (!dma->buffer) {
		ret = -ENOMEM;
		goto release_channel;
	}

	dma->fifo = fifo;
	init_completion(&dma->done);
	return dma;

release_channel:
	dma_release_channel(dma->channel);
free_context:
	kfree(dma);
	return ERR_PTR(ret);
}

static inline int sprd_spi_r4p0_dma_start(struct sprd_spi_r4p0_dma *dma,
					  const void *buffer, size_t length,
					  unsigned int bits_per_word)
{
	struct dma_slave_config config = {
		.dst_addr = dma->fifo,
		/* This provider encodes fragment lengths in bytes. */
		.src_maxburst = SPRD_SPI_R4P0_DMA_FRAGMENT_BYTES,
	};
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	unsigned long flags;
	int ret;

	if (sprd_spi_r4p0_dma_quarantined(dma))
		return -EIO;
	if (dma->state != SPRD_SPI_R4P0_DMA_IDLE)
		return -EBUSY;
	if (!buffer || !length || length > SPRD_SPI_R4P0_DMA_BUFFER_BYTES ||
	    length % SPRD_SPI_R4P0_DMA_FRAGMENT_BYTES)
		return -EINVAL;
	if (bits_per_word == 8)
		config.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	else if (bits_per_word == 16)
		config.src_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	else if (bits_per_word == 32)
		config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	else
		return -EINVAL;
	config.dst_addr_width = config.src_addr_width;

	ret = dmaengine_slave_config(dma->channel, &config);
	if (ret)
		return ret;

	memcpy(dma->buffer, buffer, length);
	dma_wmb();
	reinit_completion(&dma->done);
	dma->result = -EINPROGRESS;
	flags = SPRD_DMA_FLAGS(SPRD_DMA_CHN_MODE_NONE, SPRD_DMA_NO_TRG,
			       SPRD_DMA_FRAG_REQ, SPRD_DMA_TRANS_INT);
	desc = dmaengine_prep_slave_single(dma->channel, dma->address, length,
					   DMA_MEM_TO_DEV, flags);
	if (!desc)
		return -ENOMEM;
	desc->callback_result = sprd_spi_r4p0_dma_complete;
	desc->callback_param = dma;
	/* ACK must not alter the provider's interrupt mask passed to prep. */
	async_tx_ack(desc);
	dma->state = SPRD_SPI_R4P0_DMA_ACTIVE;
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return dma_submit_error(cookie);
	dma_async_issue_pending(dma->channel);
	return 0;
}

static inline int sprd_spi_r4p0_dma_wait(struct sprd_spi_r4p0_dma *dma,
					 ktime_t deadline)
{
	s64 remaining_us = ktime_us_delta(deadline, ktime_get());
	u64 remaining_ms;

	if (remaining_us <= 0)
		return -ETIMEDOUT;
	remaining_ms = DIV_ROUND_UP_ULL(remaining_us, USEC_PER_MSEC);
	if (remaining_ms > U32_MAX)
		return -ERANGE;
	if (!wait_for_completion_timeout(&dma->done,
					 msecs_to_jiffies(remaining_ms)))
		return -ETIMEDOUT;
	return dma->result;
}

/* The controller must disable peripheral requests before stopping the channel. */
static inline int sprd_spi_r4p0_dma_stop(struct sprd_spi_r4p0_dma *dma)
{
	int ret;

	if (sprd_spi_r4p0_dma_quarantined(dma))
		return -EIO;
	if (dma->state == SPRD_SPI_R4P0_DMA_IDLE)
		return 0;
	ret = dmaengine_terminate_sync(dma->channel);
	if (ret) {
		dma->state = SPRD_SPI_R4P0_DMA_QUARANTINED;
		return ret;
	}
	dma->state = SPRD_SPI_R4P0_DMA_IDLE;
	return 0;
}

static inline bool sprd_spi_r4p0_dma_destroy(struct sprd_spi_r4p0_dma *dma)
{
	if (!dma)
		return true;
	if (sprd_spi_r4p0_dma_stop(dma))
		return false;
	dma_free_coherent(dma->dma_dev, SPRD_SPI_R4P0_DMA_BUFFER_BYTES,
			  dma->buffer, dma->address);
	dma_release_channel(dma->channel);
	kfree(dma);
	return true;
}

#endif
