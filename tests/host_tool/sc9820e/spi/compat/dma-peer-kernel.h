/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DMA_PEER_KERNEL_H
#define FPLINUX_DMA_PEER_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <asm-generic/errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef uintptr_t dma_addr_t;
typedef uint64_t phys_addr_t;
typedef int dma_cookie_t;
typedef int64_t ktime_t;
#define EPROBE_DEFER 517
#define GFP_KERNEL 0
#define SZ_64K 65536U
#define U32_MAX UINT32_MAX
#define USEC_PER_MSEC 1000
#define BIT(n) (1UL << (n))
#define DIV_ROUND_UP_ULL(n, d) (((n) + (d) - 1) / (d))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
struct device {
	int unused;
};
struct completion {
	unsigned int done;
};
struct dma_chan {
	struct device *dev;
};
enum dma_slave_buswidth {
	DMA_SLAVE_BUSWIDTH_1_BYTE = 1,
	DMA_SLAVE_BUSWIDTH_2_BYTES = 2,
	DMA_SLAVE_BUSWIDTH_4_BYTES = 4,
};
enum dma_transfer_direction { DMA_MEM_TO_DEV = 1 };
enum dmaengine_tx_result { DMA_TRANS_NOERROR, DMA_TRANS_ABORTED };
struct dmaengine_result {
	enum dmaengine_tx_result result;
	u32 residue;
};
struct dma_slave_config {
	dma_addr_t dst_addr;
	enum dma_slave_buswidth src_addr_width, dst_addr_width;
	u32 src_maxburst;
};
struct dma_async_tx_descriptor {
	unsigned long flags;
	void (*callback_result)(void *, const struct dmaengine_result *);
	void *callback_param;
};
void *peer_kzalloc(size_t bytes);
void peer_kfree(void *memory);
#define kzalloc_obj(object) peer_kzalloc(sizeof(object))
#define kfree(memory) peer_kfree(memory)
struct dma_chan *dma_request_chan(struct device *dev, const char *name);
void dma_release_channel(struct dma_chan *channel);
struct device *dmaengine_get_dma_device(struct dma_chan *channel);
void *dma_alloc_coherent(struct device *dev, size_t bytes, dma_addr_t *address,
			 int flags);
void dma_free_coherent(struct device *dev, size_t bytes, void *buffer,
		       dma_addr_t address);
int dmaengine_slave_config(struct dma_chan *channel,
			   const struct dma_slave_config *config);
struct dma_async_tx_descriptor *
dmaengine_prep_slave_single(struct dma_chan *channel, dma_addr_t address,
			    size_t bytes, enum dma_transfer_direction direction,
			    unsigned long flags);
dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc);
void dma_async_issue_pending(struct dma_chan *channel);
int dmaengine_terminate_sync(struct dma_chan *channel);
unsigned long wait_for_completion_timeout(struct completion *done,
					  unsigned long timeout);
ktime_t ktime_get(void);
static inline s64 ktime_us_delta(ktime_t end, ktime_t start)
{
	return end - start;
}
static inline unsigned long msecs_to_jiffies(unsigned int ms)
{
	return ms;
}
static inline void init_completion(struct completion *done)
{
	done->done = 0;
}
static inline void reinit_completion(struct completion *done)
{
	done->done = 0;
}
static inline void complete(struct completion *done)
{
	done->done++;
}
static inline void dma_wmb(void)
{
}
static inline void async_tx_ack(struct dma_async_tx_descriptor *desc)
{
	desc->flags |= 2;
}
static inline int dma_submit_error(dma_cookie_t cookie)
{
	return cookie < 0 ? cookie : 0;
}
#endif
