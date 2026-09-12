/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_DMA_H
#define UMS9117_DMA_H

#include <linux/dmaengine.h>

/* Reserves the ROTA request route; release with dma_release_channel(). */
struct dma_chan *ums9117_dma_request_rota(void);

/*
 * ROTA supplies a source-first hardware list in its own register aperture.
 * This is not a RAM descriptor chain or a generic peripheral-DMA interface.
 * The caller keeps the ROTA block and payload buffers live until completion
 * or successful dmaengine_terminate_sync(). Failed termination requires the
 * caller to retain those resources until a cold boot.
 */
struct dma_async_tx_descriptor *
ums9117_dma_prep_rota(struct dma_chan *channel, dma_addr_t external_list_addr,
		      size_t length, unsigned long flags);

#endif
