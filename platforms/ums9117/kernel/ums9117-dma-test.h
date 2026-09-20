/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_DMA_TEST_H
#define UMS9117_DMA_TEST_H

#include <linux/dmaengine.h>

/* Test only: all addresses must still refer to owned DMA buffers. */
struct dma_async_tx_descriptor *
ums9117_dma_prep_alignment_fault(struct dma_chan *chan, dma_addr_t destination,
				 dma_addr_t source, size_t length,
				 unsigned long flags);

#endif
