// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/dma/ums9117-dma.h>
#include <linux/dma/ums9117-dma-test.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "virt-dma.h"

#define UMS9117_DMA_PHYS 0x20100000ULL
#define UMS9117_DMA_MMIO_BYTES 0x2100U
#define UMS9117_DMA_CHANNEL_COUNT 32U
#define UMS9117_DMA_CHANNEL_BASE 0x1000U
#define UMS9117_DMA_CHANNEL_BYTES 0x40U
#define UMS9117_DMA_UID_BASE 0x2000U
#define UMS9117_DMA_UID_COUNT 64U
#define UMS9117_DMA_GATE BIT(5)
#define UMS9117_DMA_GATE_STATE_PHYS 0x20e00000ULL
#define UMS9117_DMA_GATE_SET_PHYS 0x20e01000ULL
#define UMS9117_DMA_GATE_CLEAR_PHYS 0x20e02000ULL
#define UMS9117_DMA_CHUNK_BYTES 65535U
#define UMS9117_DMA_STOP_TIMEOUT_US 10000U
#define UMS9117_ROTA_LIST_PHYS 0x20900420ULL
#define UMS9117_ROTA_REQUEST 1U

#define UMS9117_DMA_PAUSE 0x00U
#define UMS9117_DMA_IRQ_RAW 0x10U
#define UMS9117_DMA_IRQ_MASKED 0x14U
#define UMS9117_DMA_REQUEST_STATUS 0x18U
#define UMS9117_DMA_ENABLE_STATUS 0x1cU
#define UMS9117_DMA_TWO_STAGE1 0x28U
#define UMS9117_DMA_TWO_STAGE2 0x2cU
#define UMS9117_DMA_TWO_STAGE_ENABLE BIT(24)

#define UMS9117_DMA_CH_PAUSE 0x00U
#define UMS9117_DMA_CH_REQUEST 0x04U
#define UMS9117_DMA_CH_CONFIG 0x08U
#define UMS9117_DMA_CH_INTERRUPT 0x0cU
#define UMS9117_DMA_CH_SOURCE 0x10U
#define UMS9117_DMA_CH_DESTINATION 0x14U
#define UMS9117_DMA_CH_FRAGMENT 0x18U
#define UMS9117_DMA_CH_BLOCK 0x1cU
#define UMS9117_DMA_CH_TRANSACTION 0x20U
#define UMS9117_DMA_CH_STEP 0x24U
#define UMS9117_DMA_CH_WRAP_POINTER 0x28U
#define UMS9117_DMA_CH_WRAP_TARGET 0x2cU
#define UMS9117_DMA_CH_LIST 0x30U
#define UMS9117_DMA_CH_FRAGMENT_STEP 0x34U
#define UMS9117_DMA_CH_SOURCE_BLOCK_STEP 0x38U
#define UMS9117_DMA_CH_DESTINATION_BLOCK_STEP 0x3cU

#define UMS9117_DMA_PAUSE_REQUEST BIT(0)
#define UMS9117_DMA_PAUSE_ACK BIT(2)
#define UMS9117_DMA_CHANNEL_ENABLE BIT(0)
#define UMS9117_DMA_LINK_LIST BIT(4)
#define UMS9117_DMA_SOFTWARE_REQUEST BIT(0)
#define UMS9117_DMA_CONFIG_ERROR GENMASK(23, 20)
#define UMS9117_DMA_DONT_WAIT_BLOCK BIT(24)
#define UMS9117_DMA_SOURCE_WIDTH GENMASK(31, 30)
#define UMS9117_DMA_DESTINATION_WIDTH GENMASK(29, 28)
#define UMS9117_DMA_BLOCK_REQUEST BIT(24)
#define UMS9117_DMA_EVENT_TRANSACTION BIT(2)
#define UMS9117_DMA_EVENT_LIST BIT(3)
#define UMS9117_DMA_EVENT_ERROR BIT(4)
#define UMS9117_DMA_EVENTS GENMASK(4, 0)
#define UMS9117_DMA_INTERRUPT_ENABLE \
	(UMS9117_DMA_EVENT_TRANSACTION | UMS9117_DMA_EVENT_ERROR)
#define UMS9117_DMA_INTERRUPT_CLEAR (UMS9117_DMA_EVENTS << 24)

struct ums9117_dma;

struct ums9117_dma_desc {
	struct virt_dma_desc vd;
	struct list_head error_node;
	dma_cookie_t error_cookie;
	dma_addr_t source;
	dma_addr_t destination;
	size_t length;
	size_t completed;
	u32 chunk_bytes;
	u8 width;
	bool failed;
	bool rota;
};

struct ums9117_dma_channel {
	struct virt_dma_chan vc;
	struct ums9117_dma *dma;
	void __iomem *base;
	struct ums9117_dma_desc *active;
	struct list_head failed_descriptors;
	unsigned int index;
	bool allocated;
	bool running;
	bool paused;
	bool failed;
	bool quarantined;
	bool rota;
};

struct ums9117_dma {
	struct dma_device device;
	void __iomem *base;
	void __iomem *gate_state;
	void __iomem *gate_set;
	void __iomem *gate_clear;
	int irq;
	bool gate_acquired;
	bool removing;
	atomic_t rota_channel;
	struct ums9117_dma_channel channels[UMS9117_DMA_CHANNEL_COUNT];
};

static struct ums9117_dma_channel *to_channel(struct dma_chan *channel)
{
	return container_of(channel, struct ums9117_dma_channel, vc.chan);
}

static struct ums9117_dma_desc *to_desc(struct virt_dma_desc *descriptor)
{
	return container_of(descriptor, struct ums9117_dma_desc, vd);
}

static void free_descriptor(struct virt_dma_desc *descriptor)
{
	/* Failed cookies retain their residue until the channel is released. */
	if (!to_desc(descriptor)->failed)
		kfree(to_desc(descriptor));
}

static bool channel_idle(struct ums9117_dma_channel *channel)
{
	struct ums9117_dma *dma = channel->dma;
	u32 mask = BIT(channel->index);

	return !((readl(dma->base + UMS9117_DMA_ENABLE_STATUS) |
		  readl(dma->base + UMS9117_DMA_REQUEST_STATUS)) &
		 mask);
}

static void quarantine_locked(struct ums9117_dma_channel *channel,
			      const char *reason)
{
	lockdep_assert_held(&channel->vc.lock);
	channel->failed = true;
	if (channel->quarantined)
		return;

	/* Unacknowledged DMA must outlive every address it may still access. */
	channel->quarantined = true;
	__module_get(THIS_MODULE);
	dev_err(channel->dma->device.dev,
		"channel %u did not stop (%s): enable=%08x request=%08x pause=%08x config=%08x interrupt=%08x; retained until a cold boot\n",
		channel->index, reason,
		readl(channel->dma->base + UMS9117_DMA_ENABLE_STATUS),
		readl(channel->dma->base + UMS9117_DMA_REQUEST_STATUS),
		readl(channel->base + UMS9117_DMA_CH_PAUSE),
		readl(channel->base + UMS9117_DMA_CH_CONFIG),
		readl(channel->base + UMS9117_DMA_CH_INTERRUPT));
	writel(0, channel->base + UMS9117_DMA_CH_INTERRUPT);
}

static int pause_hardware_locked(struct ums9117_dma_channel *channel)
{
	u32 value;
	int ret;

	lockdep_assert_held(&channel->vc.lock);
	if (!(readl(channel->base + UMS9117_DMA_CH_CONFIG) &
	      UMS9117_DMA_CHANNEL_ENABLE))
		return 0;

	writel(UMS9117_DMA_PAUSE_REQUEST, channel->base + UMS9117_DMA_CH_PAUSE);
	ret = readl_poll_timeout_atomic(channel->base + UMS9117_DMA_CH_PAUSE,
					value, value & UMS9117_DMA_PAUSE_ACK, 1,
					UMS9117_DMA_STOP_TIMEOUT_US);
	if (ret)
		quarantine_locked(channel, "pause acknowledgement");
	return ret;
}

static int stop_hardware_locked(struct ums9117_dma_channel *channel,
				bool rejected_alignment)
{
	u32 value;
	int ret;

	lockdep_assert_held(&channel->vc.lock);
	if (channel->quarantined)
		return -EIO;
	/* A rejected, unrequested alignment fault cannot finish a fragment. */
	if (!rejected_alignment) {
		ret = pause_hardware_locked(channel);
		if (ret)
			return ret;
	}

	value = readl(channel->base + UMS9117_DMA_CH_CONFIG);
	writel(value & ~(UMS9117_DMA_CHANNEL_ENABLE | UMS9117_DMA_CONFIG_ERROR),
	       channel->base + UMS9117_DMA_CH_CONFIG);
	writel(0, channel->base + UMS9117_DMA_CH_REQUEST);
	writel(0, channel->base + UMS9117_DMA_CH_PAUSE);
	writel(UMS9117_DMA_INTERRUPT_CLEAR,
	       channel->base + UMS9117_DMA_CH_INTERRUPT);
	if (!channel_idle(channel) ||
	    (readl(channel->base + UMS9117_DMA_CH_INTERRUPT) &
	     (UMS9117_DMA_EVENTS << 8))) {
		quarantine_locked(channel, "idle readback");
		return -EIO;
	}
	if (channel->rota)
		writel(0, channel->dma->base + UMS9117_DMA_UID_BASE +
				  (UMS9117_ROTA_REQUEST - 1) * 4);
	channel->running = false;
	return 0;
}

static void start_rota_locked(struct ums9117_dma_channel *channel)
{
	struct ums9117_dma_desc *descriptor = channel->active;
	u32 config = UMS9117_DMA_DONT_WAIT_BLOCK | UMS9117_DMA_LINK_LIST;

	lockdep_assert_held(&channel->vc.lock);
	/* ROTA produces the twelve-word, source-first descriptor at +0x420.
	 * CFG_VALID stays clear: the first peripheral request fetches that list.
	 * The initial block request differs from the fetched transaction request.
	 */
	writel(0, channel->base + UMS9117_DMA_CH_PAUSE);
	writel(0, channel->base + UMS9117_DMA_CH_REQUEST);
	writel(config, channel->base + UMS9117_DMA_CH_CONFIG);
	writel(0, channel->base + UMS9117_DMA_CH_SOURCE);
	writel(0, channel->base + UMS9117_DMA_CH_DESTINATION);
	writel(UMS9117_DMA_BLOCK_REQUEST,
	       channel->base + UMS9117_DMA_CH_FRAGMENT);
	writel(32, channel->base + UMS9117_DMA_CH_BLOCK);
	writel(512, channel->base + UMS9117_DMA_CH_TRANSACTION);
	writel(0, channel->base + UMS9117_DMA_CH_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_WRAP_POINTER);
	writel(0, channel->base + UMS9117_DMA_CH_WRAP_TARGET);
	writel(lower_32_bits(descriptor->source),
	       channel->base + UMS9117_DMA_CH_LIST);
	writel(0, channel->base + UMS9117_DMA_CH_FRAGMENT_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_SOURCE_BLOCK_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_DESTINATION_BLOCK_STEP);
	writel(UMS9117_DMA_INTERRUPT_CLEAR | UMS9117_DMA_EVENT_LIST |
		       UMS9117_DMA_EVENT_ERROR,
	       channel->base + UMS9117_DMA_CH_INTERRUPT);
	writel(channel->index + 1, channel->dma->base + UMS9117_DMA_UID_BASE +
					   (UMS9117_ROTA_REQUEST - 1) * 4);
	channel->running = true;
	writel(config | UMS9117_DMA_CHANNEL_ENABLE,
	       channel->base + UMS9117_DMA_CH_CONFIG);
	/* The client starts ROTA only after issue_pending() returns. */
}

static void start_chunk_locked(struct ums9117_dma_channel *channel)
{
	struct ums9117_dma_desc *descriptor = channel->active;
	u32 step = 1U << descriptor->width;
	u32 fragment;

	lockdep_assert_held(&channel->vc.lock);
	descriptor->chunk_bytes =
		min_t(size_t, descriptor->length - descriptor->completed,
		      UMS9117_DMA_CHUNK_BYTES & ~(step - 1));
	fragment =
		FIELD_PREP(UMS9117_DMA_SOURCE_WIDTH, descriptor->width) |
		FIELD_PREP(UMS9117_DMA_DESTINATION_WIDTH, descriptor->width) |
		UMS9117_DMA_BLOCK_REQUEST | descriptor->chunk_bytes;

	writel(0, channel->base + UMS9117_DMA_CH_PAUSE);
	writel(0, channel->base + UMS9117_DMA_CH_REQUEST);
	writel(UMS9117_DMA_DONT_WAIT_BLOCK,
	       channel->base + UMS9117_DMA_CH_CONFIG);
	writel(lower_32_bits(descriptor->source + descriptor->completed),
	       channel->base + UMS9117_DMA_CH_SOURCE);
	writel(lower_32_bits(descriptor->destination + descriptor->completed),
	       channel->base + UMS9117_DMA_CH_DESTINATION);
	writel(fragment, channel->base + UMS9117_DMA_CH_FRAGMENT);
	writel(descriptor->chunk_bytes, channel->base + UMS9117_DMA_CH_BLOCK);
	writel(descriptor->chunk_bytes,
	       channel->base + UMS9117_DMA_CH_TRANSACTION);
	writel(step | (step << 16), channel->base + UMS9117_DMA_CH_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_WRAP_POINTER);
	writel(0, channel->base + UMS9117_DMA_CH_WRAP_TARGET);
	writel(0, channel->base + UMS9117_DMA_CH_LIST);
	writel(0, channel->base + UMS9117_DMA_CH_FRAGMENT_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_SOURCE_BLOCK_STEP);
	writel(0, channel->base + UMS9117_DMA_CH_DESTINATION_BLOCK_STEP);
	writel(UMS9117_DMA_INTERRUPT_CLEAR | UMS9117_DMA_INTERRUPT_ENABLE,
	       channel->base + UMS9117_DMA_CH_INTERRUPT);

	/* Descriptor and mapped payload stores precede the DMA request. */
	channel->running = true;
	writel(UMS9117_DMA_DONT_WAIT_BLOCK | UMS9117_DMA_CHANNEL_ENABLE,
	       channel->base + UMS9117_DMA_CH_CONFIG);
	writel(UMS9117_DMA_SOFTWARE_REQUEST,
	       channel->base + UMS9117_DMA_CH_REQUEST);
}

static void start_next_locked(struct ums9117_dma_channel *channel)
{
	struct virt_dma_desc *descriptor;

	lockdep_assert_held(&channel->vc.lock);
	if (channel->paused || channel->failed ||
	    READ_ONCE(channel->dma->removing) || channel->running)
		return;
	if (!channel->active) {
		descriptor = vchan_next_desc(&channel->vc);
		if (!descriptor)
			return;
		list_del(&descriptor->node);
		channel->active = to_desc(descriptor);
	}
	if (channel->active->rota)
		start_rota_locked(channel);
	else
		start_chunk_locked(channel);
}

static void abort_descriptor_locked(struct ums9117_dma_channel *channel,
				    struct ums9117_dma_desc *descriptor)
{
	descriptor->failed = true;
	descriptor->error_cookie = descriptor->vd.tx.cookie;
	list_add_tail(&descriptor->error_node, &channel->failed_descriptors);
	descriptor->vd.tx_result.result = DMA_TRANS_ABORTED;
	descriptor->vd.tx_result.residue =
		descriptor->length - descriptor->completed;
	vchan_cookie_complete(&descriptor->vd);
}

static void fail_queue_locked(struct ums9117_dma_channel *channel)
{
	struct virt_dma_desc *descriptor;

	lockdep_assert_held(&channel->vc.lock);
	channel->failed = true;
	if (channel->active) {
		abort_descriptor_locked(channel, channel->active);
		channel->active = NULL;
	}
	vchan_issue_pending(&channel->vc);
	while ((descriptor = vchan_next_desc(&channel->vc))) {
		list_del(&descriptor->node);
		abort_descriptor_locked(channel, to_desc(descriptor));
	}
}

static irqreturn_t ums9117_dma_irq(int irq, void *data)
{
	struct ums9117_dma *dma = data;
	struct ums9117_dma_channel *channel;
	struct ums9117_dma_desc *descriptor;
	u32 pending = readl(dma->base + UMS9117_DMA_IRQ_MASKED);
	u32 events;
	u32 config;
	bool rejected_alignment;
	unsigned int index;

	if (!pending)
		return IRQ_NONE;
	while (pending) {
		index = __ffs(pending);
		pending &= ~BIT(index);
		channel = &dma->channels[index];
		spin_lock(&channel->vc.lock);
		events =
			(readl(channel->base + UMS9117_DMA_CH_INTERRUPT) >> 8) &
			UMS9117_DMA_EVENTS;
		if (!events)
			goto unlock;
		config = readl(channel->base + UMS9117_DMA_CH_CONFIG);
		rejected_alignment =
			events == UMS9117_DMA_EVENT_ERROR &&
			FIELD_GET(UMS9117_DMA_CONFIG_ERROR, config) == 8 &&
			!(readl(dma->base + UMS9117_DMA_REQUEST_STATUS) &
			  BIT(index)) &&
			!(readl(channel->base + UMS9117_DMA_CH_REQUEST) &
			  UMS9117_DMA_SOFTWARE_REQUEST);
		if (events & UMS9117_DMA_EVENT_ERROR) {
			dev_err_ratelimited(
				dma->device.dev,
				"channel %u configuration error: events=%02x config=%08x software-request=%08x\n",
				index, events, config,
				readl(channel->base + UMS9117_DMA_CH_REQUEST));
		}
		writel(events << 24, channel->base + UMS9117_DMA_CH_INTERRUPT);
		if (stop_hardware_locked(channel, rejected_alignment))
			goto unlock;
		descriptor = channel->active;
		if (!descriptor)
			goto unlock;
		if ((events & UMS9117_DMA_EVENT_ERROR) ||
		    !(events &
		      (descriptor->rota ? UMS9117_DMA_EVENT_LIST :
					  UMS9117_DMA_EVENT_TRANSACTION))) {
			dev_err_ratelimited(
				dma->device.dev,
				"channel %u transfer failed: events=%02x config=%08x\n",
				index, events, config);
			fail_queue_locked(channel);
			goto unlock;
		}
		if (descriptor->rota)
			descriptor->completed = descriptor->length;
		else
			descriptor->completed += descriptor->chunk_bytes;
		if (descriptor->completed == descriptor->length) {
			vchan_cookie_complete(&descriptor->vd);
			channel->active = NULL;
		}
		start_next_locked(channel);
unlock:
		spin_unlock(&channel->vc.lock);
	}
	return IRQ_HANDLED;
}

static dma_cookie_t submit_descriptor(struct dma_async_tx_descriptor *tx)
{
	struct ums9117_dma_channel *channel = to_channel(tx->chan);
	struct virt_dma_desc *descriptor =
		container_of(tx, struct virt_dma_desc, tx);
	unsigned long flags;
	dma_cookie_t cookie;

	spin_lock_irqsave(&channel->vc.lock, flags);
	if (channel->failed || READ_ONCE(channel->dma->removing) ||
	    !channel->allocated) {
		cookie = -EIO;
	} else {
		cookie = dma_cookie_assign(tx);
		list_move_tail(&descriptor->node, &channel->vc.desc_submitted);
	}
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return cookie;
}

static struct dma_async_tx_descriptor *
ums9117_dma_prep_memcpy(struct dma_chan *chan, dma_addr_t destination,
			dma_addr_t source, size_t length, unsigned long flags)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	struct ums9117_dma_desc *descriptor;
	struct dma_async_tx_descriptor *tx;
	unsigned long lock_flags;
	bool available;

	if (!length || length > U32_MAX || upper_32_bits(source) ||
	    upper_32_bits(destination) || length - 1 > U32_MAX - source ||
	    length - 1 > U32_MAX - destination)
		return NULL;
	spin_lock_irqsave(&channel->vc.lock, lock_flags);
	available = channel->allocated && !channel->rota && !channel->failed &&
		    !READ_ONCE(channel->dma->removing);
	spin_unlock_irqrestore(&channel->vc.lock, lock_flags);
	if (!available)
		return NULL;
	descriptor = kzalloc(sizeof(*descriptor), GFP_NOWAIT);
	if (!descriptor)
		return NULL;
	descriptor->source = source;
	descriptor->destination = destination;
	descriptor->length = length;
	if (IS_ALIGNED(source | destination | length, 4))
		descriptor->width = 2;
	else if (IS_ALIGNED(source | destination | length, 2))
		descriptor->width = 1;
	tx = vchan_tx_prep(&channel->vc, &descriptor->vd, flags);
	tx->tx_submit = submit_descriptor;
	return tx;
}

static bool rota_channel_filter(struct dma_chan *chan, void *parameter)
{
	struct ums9117_dma_channel *channel;
	int *error = parameter;

	if (chan->device->device_prep_dma_memcpy != ums9117_dma_prep_memcpy)
		return false;
	channel = to_channel(chan);
	*error = -EBUSY;
	return !READ_ONCE(channel->dma->removing) &&
	       atomic_read(&channel->dma->rota_channel) == -1;
}

struct dma_chan *ums9117_dma_request_rota(void)
{
	struct ums9117_dma_channel *channel;
	struct dma_chan *chan;
	dma_cap_mask_t mask;
	unsigned long flags;
	int error = -EPROBE_DEFER;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_channel(mask, rota_channel_filter, &error);
	if (!chan)
		return ERR_PTR(error);
	channel = to_channel(chan);
	/* There is one ROTA request wire, independent of the free channel count. */
	if (atomic_cmpxchg(&channel->dma->rota_channel, -1, channel->index) !=
	    -1) {
		dma_release_channel(chan);
		return ERR_PTR(-EBUSY);
	}
	spin_lock_irqsave(&channel->vc.lock, flags);
	channel->rota = true;
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return chan;
}
EXPORT_SYMBOL_GPL(ums9117_dma_request_rota);

struct dma_async_tx_descriptor *
ums9117_dma_prep_rota(struct dma_chan *chan, dma_addr_t external_list_addr,
		      size_t length, unsigned long flags)
{
	struct ums9117_dma_channel *channel;
	struct ums9117_dma_desc *descriptor;
	struct dma_async_tx_descriptor *tx;
	unsigned long lock_flags;
	bool available;

	if (chan->device->device_prep_dma_memcpy != ums9117_dma_prep_memcpy ||
	    external_list_addr != UMS9117_ROTA_LIST_PHYS || !length ||
	    length > U32_MAX)
		return NULL;
	channel = to_channel(chan);
	spin_lock_irqsave(&channel->vc.lock, lock_flags);
	available = channel->allocated && channel->rota && !channel->failed &&
		    !READ_ONCE(channel->dma->removing);
	spin_unlock_irqrestore(&channel->vc.lock, lock_flags);
	if (!available)
		return NULL;
	descriptor = kzalloc(sizeof(*descriptor), GFP_NOWAIT);
	if (!descriptor)
		return NULL;
	descriptor->source = external_list_addr;
	descriptor->length = length;
	descriptor->rota = true;
	tx = vchan_tx_prep(&channel->vc, &descriptor->vd, flags);
	tx->tx_submit = submit_descriptor;
	return tx;
}
EXPORT_SYMBOL_GPL(ums9117_dma_prep_rota);

#if IS_ENABLED(CONFIG_UMS9117_DMA_TEST)
struct dma_async_tx_descriptor *
ums9117_dma_prep_alignment_fault(struct dma_chan *chan, dma_addr_t destination,
				 dma_addr_t source, size_t length,
				 unsigned long flags)
{
	struct dma_async_tx_descriptor *tx;
	struct virt_dma_desc *descriptor;

	if (chan->device->device_prep_dma_memcpy != ums9117_dma_prep_memcpy ||
	    !(source & 1) || (destination & 1) || (length & 1))
		return NULL;
	tx = ums9117_dma_prep_memcpy(chan, destination, source, length, flags);
	if (!tx)
		return NULL;
	/* The documented halfword alignment fault occurs before payload access. */
	descriptor = container_of(tx, struct virt_dma_desc, tx);
	to_desc(descriptor)->width = 1;
	return tx;
}
EXPORT_SYMBOL_GPL(ums9117_dma_prep_alignment_fault);
#endif

static void ums9117_dma_issue_pending(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	unsigned long flags;

	spin_lock_irqsave(&channel->vc.lock, flags);
	if (vchan_issue_pending(&channel->vc))
		start_next_locked(channel);
	spin_unlock_irqrestore(&channel->vc.lock, flags);
}

static enum dma_status ums9117_dma_tx_status(struct dma_chan *chan,
					     dma_cookie_t cookie,
					     struct dma_tx_state *state)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	struct virt_dma_desc *descriptor;
	struct ums9117_dma_desc *failed;
	unsigned long flags;
	enum dma_status status;

	spin_lock_irqsave(&channel->vc.lock, flags);
	status = dma_cookie_status(chan, cookie, state);
	list_for_each_entry(failed, &channel->failed_descriptors, error_node) {
		if (failed->error_cookie == cookie) {
			dma_set_residue(state,
					failed->length - failed->completed);
			status = DMA_ERROR;
			goto unlock;
		}
	}
	if (status == DMA_COMPLETE)
		goto unlock;
	if (channel->active && channel->active->vd.tx.cookie == cookie) {
		dma_set_residue(state, channel->active->length -
					       channel->active->completed);
	} else {
		list_for_each_entry(descriptor, &channel->vc.desc_issued,
				    node) {
			if (descriptor->tx.cookie == cookie) {
				dma_set_residue(state,
						to_desc(descriptor)->length);
				goto paused;
			}
		}
		list_for_each_entry(descriptor, &channel->vc.desc_submitted,
				    node) {
			if (descriptor->tx.cookie == cookie) {
				dma_set_residue(state,
						to_desc(descriptor)->length);
				break;
			}
		}
	}
paused:
	if (channel->failed)
		status = DMA_ERROR;
	else if (channel->paused)
		status = DMA_PAUSED;
unlock:
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return status;
}

static int ums9117_dma_pause(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&channel->vc.lock, flags);
	if (channel->failed || READ_ONCE(channel->dma->removing)) {
		ret = -EIO;
	} else {
		channel->paused = true;
		if (channel->running)
			ret = pause_hardware_locked(channel);
	}
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return ret;
}

static int ums9117_dma_resume(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&channel->vc.lock, flags);
	if (channel->failed || READ_ONCE(channel->dma->removing)) {
		ret = -EIO;
	} else {
		channel->paused = false;
		if (channel->running)
			writel(0, channel->base + UMS9117_DMA_CH_PAUSE);
		else
			start_next_locked(channel);
	}
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return ret;
}

static int ums9117_dma_terminate_all(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	unsigned long flags;
	LIST_HEAD(descriptors);
	int ret;

	spin_lock_irqsave(&channel->vc.lock, flags);
	ret = stop_hardware_locked(channel, false);
	if (ret)
		goto unlock;
	if (channel->active) {
		list_add_tail(&channel->active->vd.node, &descriptors);
		channel->active = NULL;
	}
	vchan_get_all_descriptors(&channel->vc, &descriptors);
	list_splice_tail_init(&descriptors, &channel->vc.desc_terminated);
	channel->paused = false;
unlock:
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return ret;
}

static void ums9117_dma_synchronize(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);

	synchronize_irq(channel->dma->irq);
	vchan_synchronize(&channel->vc);
}

static int ums9117_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	unsigned long flags;
	unsigned int offset;
	int ret = 1;

	spin_lock_irqsave(&channel->vc.lock, flags);
	if (channel->failed || READ_ONCE(channel->dma->removing) ||
	    !channel_idle(channel)) {
		ret = -EBUSY;
		goto unlock;
	}
	for (offset = 0; offset < UMS9117_DMA_CHANNEL_BYTES; offset += 4)
		writel(offset == UMS9117_DMA_CH_INTERRUPT ?
			       UMS9117_DMA_INTERRUPT_CLEAR :
			       0,
		       channel->base + offset);
	dma_cookie_init(chan);
	channel->allocated = true;
unlock:
	spin_unlock_irqrestore(&channel->vc.lock, flags);
	return ret;
}

static void ums9117_dma_free_chan_resources(struct dma_chan *chan)
{
	struct ums9117_dma_channel *channel = to_channel(chan);
	struct ums9117_dma_desc *descriptor;
	struct ums9117_dma_desc *next;
	unsigned long flags;

	if (ums9117_dma_terminate_all(chan))
		return;
	ums9117_dma_synchronize(chan);
	vchan_free_chan_resources(&channel->vc);
	spin_lock_irqsave(&channel->vc.lock, flags);
	list_for_each_entry_safe(descriptor, next, &channel->failed_descriptors,
				 error_node) {
		list_del(&descriptor->error_node);
		kfree(descriptor);
	}
	channel->allocated = false;
	channel->failed = false;
	if (channel->rota) {
		channel->rota = false;
		atomic_set(&channel->dma->rota_channel, -1);
	}
	spin_unlock_irqrestore(&channel->vc.lock, flags);
}

static void __iomem *map_shared_gate(struct platform_device *pdev,
				     const char *name, resource_size_t address)
{
	struct resource *resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;

	if (!resource || resource->start != address ||
	    resource_size(resource) != 4)
		return IOMEM_ERR_PTR(-EINVAL);
	/* Atomic aliases deliberately share words with display, USB and MMC. */
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static void restore_gate(struct ums9117_dma *dma)
{
	if (!dma->gate_acquired)
		return;
	writel(UMS9117_DMA_GATE, dma->gate_clear);
	if (readl(dma->gate_state) & UMS9117_DMA_GATE)
		dev_err(dma->device.dev,
			"could not restore AP DMA clock gate\n");
	else
		dma->gate_acquired = false;
}

static int validate_idle_controller(struct ums9117_dma *dma)
{
	u32 enabled = readl(dma->base + UMS9117_DMA_ENABLE_STATUS);
	u32 requested = readl(dma->base + UMS9117_DMA_REQUEST_STATUS);
	u32 pause = readl(dma->base + UMS9117_DMA_PAUSE);
	u32 group1 = readl(dma->base + UMS9117_DMA_TWO_STAGE1);
	u32 group2 = readl(dma->base + UMS9117_DMA_TWO_STAGE2);
	u32 value;
	unsigned int index;

	dev_dbg(dma->device.dev,
		"inherited enable=%08x request=%08x pause=%08x groups=%08x/%08x raw=%08x\n",
		enabled, requested, pause, group1, group2,
		readl(dma->base + UMS9117_DMA_IRQ_RAW));
	if (enabled || requested) {
		/* Do not gate an inherited bus master that may still be active. */
		dma->gate_acquired = false;
		dev_err(dma->device.dev,
			"inherited DMA activity: enable=%08x request=%08x\n",
			enabled, requested);
		return -EBUSY;
	}
	if ((pause & UMS9117_DMA_PAUSE_REQUEST) ||
	    ((group1 | group2) & UMS9117_DMA_TWO_STAGE_ENABLE))
		return -EBUSY;
	for (index = 0; index < UMS9117_DMA_UID_COUNT; ++index) {
		value = readl(dma->base + UMS9117_DMA_UID_BASE + index * 4);
		if (value) {
			dev_err(dma->device.dev,
				"inherited request %u still maps to channel %u\n",
				index + 1, value);
			return -EBUSY;
		}
	}
	return 0;
}

static void release_device(struct dma_device *device)
{
	struct ums9117_dma *dma =
		container_of(device, struct ums9117_dma, device);

	kfree(dma);
}

static int ums9117_dma_probe(struct platform_device *pdev)
{
	struct ums9117_dma_channel *channel;
	struct resource *resource;
	struct ums9117_dma *dma;
	unsigned int index;
	u32 state;
	int ret;

	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	dma->device.dev = &pdev->dev;
	atomic_set(&dma->rota_channel, -1);
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma");
	if (!resource || resource->start != UMS9117_DMA_PHYS ||
	    resource_size(resource) != UMS9117_DMA_MMIO_BYTES) {
		ret = -EINVAL;
		goto free_device;
	}
	dma->base = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(dma->base)) {
		ret = PTR_ERR(dma->base);
		goto free_device;
	}
	dma->gate_state = map_shared_gate(pdev, "ap-ahb-state",
					  UMS9117_DMA_GATE_STATE_PHYS);
	dma->gate_set =
		map_shared_gate(pdev, "ap-ahb-set", UMS9117_DMA_GATE_SET_PHYS);
	dma->gate_clear = map_shared_gate(pdev, "ap-ahb-clear",
					  UMS9117_DMA_GATE_CLEAR_PHYS);
	if (IS_ERR(dma->gate_state) || IS_ERR(dma->gate_set) ||
	    IS_ERR(dma->gate_clear)) {
		ret = IS_ERR(dma->gate_state) ? PTR_ERR(dma->gate_state) :
		      IS_ERR(dma->gate_set)   ? PTR_ERR(dma->gate_set) :
						PTR_ERR(dma->gate_clear);
		goto free_device;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto free_device;
	dma->irq = platform_get_irq(pdev, 0);
	if (dma->irq < 0) {
		ret = dma->irq;
		goto free_device;
	}
	state = readl(dma->gate_state);
	dma->gate_acquired = !(state & UMS9117_DMA_GATE);
	if (dma->gate_acquired)
		writel(UMS9117_DMA_GATE, dma->gate_set);
	if (!(readl(dma->gate_state) & UMS9117_DMA_GATE)) {
		ret = -EIO;
		goto gate_off;
	}
	ret = validate_idle_controller(dma);
	if (ret)
		goto gate_off;
	INIT_LIST_HEAD(&dma->device.channels);
	for (index = 0; index < UMS9117_DMA_CHANNEL_COUNT; ++index) {
		channel = &dma->channels[index];
		channel->dma = dma;
		channel->index = index;
		channel->base = dma->base + UMS9117_DMA_CHANNEL_BASE +
				index * UMS9117_DMA_CHANNEL_BYTES;
		channel->vc.desc_free = free_descriptor;
		INIT_LIST_HEAD(&channel->failed_descriptors);
		vchan_init(&channel->vc, &dma->device);
		writel(UMS9117_DMA_INTERRUPT_CLEAR,
		       channel->base + UMS9117_DMA_CH_INTERRUPT);
	}
	ret = devm_request_irq(&pdev->dev, dma->irq, ums9117_dma_irq, 0,
			       dev_name(&pdev->dev), dma);
	if (ret)
		goto gate_off;
	dma_cap_set(DMA_MEMCPY, dma->device.cap_mask);
	dma->device.device_alloc_chan_resources =
		ums9117_dma_alloc_chan_resources;
	dma->device.device_free_chan_resources =
		ums9117_dma_free_chan_resources;
	dma->device.device_prep_dma_memcpy = ums9117_dma_prep_memcpy;
	dma->device.device_issue_pending = ums9117_dma_issue_pending;
	dma->device.device_tx_status = ums9117_dma_tx_status;
	dma->device.device_pause = ums9117_dma_pause;
	dma->device.device_resume = ums9117_dma_resume;
	dma->device.device_terminate_all = ums9117_dma_terminate_all;
	dma->device.device_synchronize = ums9117_dma_synchronize;
	dma->device.device_release = release_device;
	dma->device.copy_align = DMAENGINE_ALIGN_1_BYTE;
	dma->device.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	platform_set_drvdata(pdev, dma);
	ret = dma_async_device_register(&dma->device);
	if (ret) {
		devm_free_irq(&pdev->dev, dma->irq, dma);
		goto gate_off;
	}
	dev_info(&pdev->dev, "%u memory-copy channels registered\n",
		 UMS9117_DMA_CHANNEL_COUNT);
	return 0;

gate_off:
	restore_gate(dma);
free_device:
	kfree(dma);
	return dev_err_probe(&pdev->dev, ret, "could not initialize AP DMA\n");
}

static bool quiesce_device(struct ums9117_dma *dma)
{
	unsigned int index;
	bool stopped = true;

	WRITE_ONCE(dma->removing, true);
	for (index = 0; index < UMS9117_DMA_CHANNEL_COUNT; ++index) {
		if (ums9117_dma_terminate_all(&dma->channels[index].vc.chan))
			stopped = false;
		ums9117_dma_synchronize(&dma->channels[index].vc.chan);
	}
	disable_irq(dma->irq);
	if (!stopped) {
		dev_err(dma->device.dev,
			"AP DMA state retained; cold boot required\n");
		return false;
	}
	restore_gate(dma);
	return true;
}

static void ums9117_dma_shutdown(struct platform_device *pdev)
{
	quiesce_device(platform_get_drvdata(pdev));
}

static void ums9117_dma_remove(struct platform_device *pdev)
{
	struct ums9117_dma *dma = platform_get_drvdata(pdev);

	if (!quiesce_device(dma))
		return;
	devm_free_irq(&pdev->dev, dma->irq, dma);
	dma_async_device_unregister(&dma->device);
}

static const struct of_device_id ums9117_dma_of_match[] = {
	{ .compatible = "sprd,ums9117-dma" },
	{},
};
MODULE_DEVICE_TABLE(of, ums9117_dma_of_match);

static struct platform_driver ums9117_dma_driver = {
	.probe = ums9117_dma_probe,
	.remove = ums9117_dma_remove,
	.shutdown = ums9117_dma_shutdown,
	.driver = {
		.name = "ums9117-dma",
		.of_match_table = ums9117_dma_of_match,
		/* DMAengine module references protect removal while clients exist. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ums9117_dma_driver);

MODULE_DESCRIPTION("Unisoc UMS9117 AP memory-copy DMA controller");
MODULE_LICENSE("GPL");
