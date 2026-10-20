// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include "spi-sprd-r4p0-dma.h"

static struct {
	struct device device;
	struct dma_chan channel;
	struct dma_async_tx_descriptor desc;
	struct dma_slave_config config;
	void *context;
	void *coherent;
	dma_addr_t source;
	size_t length;
	unsigned long prep_flags;
	unsigned int submissions, stops, releases, frees;
	int mode;
	int request_error;
	bool allocation_error, context_alive, active, pending, submit_error;
	bool callback_on_stop;
	u8 output[72];
} peer;

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

void *peer_kzalloc(size_t bytes)
{
	peer.context = calloc(1, bytes);
	peer.context_alive = peer.context != NULL;
	return peer.context;
}

void peer_kfree(void *memory)
{
	CHECK(memory == peer.context && peer.context_alive);
	CHECK(!peer.active && !peer.pending);
	peer.context_alive = false;
	peer.frees++;
	free(memory);
}

struct dma_chan *dma_request_chan(struct device *dev, const char *name)
{
	CHECK(dev == &peer.device && !strcmp(name, "tx_chn"));
	return peer.request_error ? ERR_PTR(peer.request_error) : &peer.channel;
}

void dma_release_channel(struct dma_chan *channel)
{
	CHECK(channel == &peer.channel && !peer.active && !peer.pending);
	peer.releases++;
}

struct device *dmaengine_get_dma_device(struct dma_chan *channel)
{
	CHECK(channel == &peer.channel);
	return &peer.device;
}

void *dma_alloc_coherent(struct device *dev, size_t bytes, dma_addr_t *address,
			 int flags)
{
	(void)flags;
	CHECK(dev == &peer.device && bytes == 65536);
	if (peer.allocation_error)
		return NULL;
	peer.coherent = malloc(bytes);
	CHECK(peer.coherent);
	memset(peer.coherent, 0xa5, bytes);
	*address = (uintptr_t)peer.coherent;
	return peer.coherent;
}

void dma_free_coherent(struct device *dev, size_t bytes, void *buffer,
		       dma_addr_t address)
{
	CHECK(dev == &peer.device && bytes == 65536);
	CHECK(buffer == peer.coherent && address == (uintptr_t)buffer);
	CHECK(!peer.active && !peer.pending && peer.context_alive);
	free(buffer);
	peer.coherent = NULL;
}

int dmaengine_slave_config(struct dma_chan *channel,
			   const struct dma_slave_config *config)
{
	CHECK(channel == &peer.channel);
	peer.config = *config;
	return 0;
}

struct dma_async_tx_descriptor *
dmaengine_prep_slave_single(struct dma_chan *channel, dma_addr_t address,
			    size_t bytes, enum dma_transfer_direction direction,
			    unsigned long flags)
{
	CHECK(channel == &peer.channel && direction == DMA_MEM_TO_DEV);
	CHECK(address == (uintptr_t)peer.coherent && bytes <= 65536);
	peer.source = address;
	peer.length = bytes;
	peer.prep_flags = flags;
	peer.desc.flags = flags;
	peer.pending = true;
	return &peer.desc;
}

dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc)
{
	CHECK(desc == &peer.desc && peer.context_alive);
	peer.submissions++;
	return peer.submit_error ? -EIO : 1;
}

void dma_async_issue_pending(struct dma_chan *channel)
{
	CHECK(channel == &peer.channel);
	peer.active = true;
}

static void callback(enum dmaengine_tx_result result)
{
	const struct dmaengine_result status = { .result = result };

	CHECK(peer.context_alive);
	peer.desc.callback_result(peer.desc.callback_param, &status);
}

int dmaengine_terminate_sync(struct dma_chan *channel)
{
	CHECK(channel == &peer.channel && peer.context_alive);
	peer.stops++;
	if (peer.mode == 3)
		return -EIO;
	if (peer.callback_on_stop)
		callback(DMA_TRANS_ABORTED);
	peer.active = false;
	peer.pending = false;
	return 0;
}

unsigned long wait_for_completion_timeout(struct completion *done,
					  unsigned long timeout)
{
	CHECK(timeout > 0 && timeout <= 500);
	if (peer.mode == 1) {
		CHECK(peer.length == sizeof(peer.output));
		memcpy(peer.output, (void *)peer.source, peer.length);
		callback(DMA_TRANS_NOERROR);
	} else if (peer.mode == 2) {
		callback(DMA_TRANS_ABORTED);
	}
	return done->done ? 1 : 0;
}

ktime_t ktime_get(void)
{
	return 10;
}

int main(int argc, char **argv)
{
	static const u8 expected[] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
		0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
		0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
		0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
		0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
		0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
		0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
		0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	};
	struct sprd_spi_r4p0_dma *dma;
	u8 source[72];
	unsigned int bits = 16;
	int ret;

	CHECK(argc == 2);
	memcpy(source, expected, sizeof(source));
	if (!strcmp(argv[1], "request-error"))
		peer.request_error = -EPROBE_DEFER;
	if (!strcmp(argv[1], "allocation-error"))
		peer.allocation_error = true;
	dma = sprd_spi_r4p0_dma_create(&peer.device, 0x70b00000);
	if (peer.request_error || peer.allocation_error) {
		CHECK(IS_ERR(dma));
		CHECK(PTR_ERR(dma) ==
		      (peer.request_error ? -EPROBE_DEFER : -ENOMEM));
		CHECK(peer.frees == 1 &&
		      peer.releases == (peer.allocation_error ? 1U : 0U));
		return 0;
	}
	CHECK(!IS_ERR(dma));
	if (!strcmp(argv[1], "invalid-length")) {
		CHECK(sprd_spi_r4p0_dma_start(dma, source, 70, 16) == -EINVAL);
		CHECK(sprd_spi_r4p0_dma_start(dma, source, 65544, 16) ==
		      -EINVAL);
		CHECK(sprd_spi_r4p0_dma_start(dma, source, 72, 24) == -EINVAL);
		CHECK(peer.submissions == 0);
		CHECK(sprd_spi_r4p0_dma_destroy(dma));
		return 0;
	}
	if (!strcmp(argv[1], "width8"))
		bits = 8;
	if (!strcmp(argv[1], "width32"))
		bits = 32;
	if (!strcmp(argv[1], "width8") || !strcmp(argv[1], "width16") ||
	    !strcmp(argv[1], "width32"))
		peer.mode = 1;
	else if (!strcmp(argv[1], "completion-error"))
		peer.mode = 2;
	else if (!strcmp(argv[1], "stop-error"))
		peer.mode = 3;
	else if (!strcmp(argv[1], "submit-error"))
		peer.submit_error = true;
	peer.callback_on_stop = !peer.submit_error && peer.mode != 1 &&
				peer.mode != 2;
	ret = sprd_spi_r4p0_dma_start(dma, source, sizeof(source), bits);
	CHECK(ret == (peer.submit_error ? -EIO : 0));
	CHECK(peer.prep_flags == 4 && peer.desc.flags == 6);
	CHECK(peer.config.dst_addr == 0x70b00000);
	CHECK(peer.config.src_maxburst == 8);
	CHECK(peer.config.src_addr_width == (bits == 8	? 1 :
					     bits == 16 ? 2 :
							  4));
	CHECK(peer.config.dst_addr_width == (bits == 8	? 1 :
					     bits == 16 ? 2 :
							  4));
	memset(source, 0, sizeof(source));
	if (!peer.submit_error) {
		ret = sprd_spi_r4p0_dma_wait(dma, 500010);
		CHECK(ret == (peer.mode == 1 ? 0 :
			      peer.mode == 2 ? -EIO :
					       -ETIMEDOUT));
	}
	if (peer.mode == 1)
		CHECK(!memcmp(peer.output, expected, sizeof(expected)));
	ret = sprd_spi_r4p0_dma_stop(dma);
	if (peer.mode == 3) {
		CHECK(ret == -EIO && sprd_spi_r4p0_dma_quarantined(dma));
		CHECK(!sprd_spi_r4p0_dma_destroy(dma));
		CHECK(peer.frees == 0 && peer.releases == 0 && peer.coherent);
		CHECK(sprd_spi_r4p0_dma_start(dma, source, sizeof(source),
					      bits) == -EIO);
		CHECK(peer.submissions == 1);
		callback(DMA_TRANS_NOERROR);
		return 0;
	}
	CHECK(ret == 0 && peer.stops == 1);
	CHECK(sprd_spi_r4p0_dma_destroy(dma));
	CHECK(peer.frees == 1 && peer.releases == 1 && !peer.coherent);
	return 0;
}
