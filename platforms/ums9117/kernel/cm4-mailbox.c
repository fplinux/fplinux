// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 CM4 channel-4 handshake and ring0 H4 stream. */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "cm4-mailbox.h"

#define UMS9117_MBOX_ID 0x00
#define UMS9117_MBOX_MSG_L 0x04
#define UMS9117_MBOX_MSG_H 0x08
#define UMS9117_MBOX_TRI 0x0c
#define UMS9117_MBOX_FIFO_RST 0x10
#define UMS9117_MBOX_FIFO_STS 0x14
#define UMS9117_MBOX_IRQ_STS 0x18
#define UMS9117_MBOX_IRQ_MSK 0x1c
#define UMS9117_MBOX_FIFO_DEPTH 0x24
#define UMS9117_MBOX_VERSION 0x28

#define UMS9117_MBOX_MASK_ALL 0xffffffffU
#define UMS9117_MBOX_SEND_IDLE 0xfffffffcU
#define UMS9117_MBOX_SEND_DELIVERY 0xfffffffbU
#define UMS9117_MBOX_RECEIVE_ARMED 0xfffffffeU
#define UMS9117_MBOX_PEER_PENDING BIT(25)
#define UMS9117_MBOX_PEER_DELIVERED BIT(17)
#define UMS9117_MBOX_BLOCK_MASK 0x000000ffU
#define UMS9117_MBOX_CLEAR_MASK 0x00ffff00U
#define UMS9117_MBOX_SOURCE_MASK 0x0000ff00U
#define UMS9117_MBOX_FIFO_UNAVAILABLE (BIT(31) | BIT(30))
#define UMS9117_MBOX_EVENT_LIMIT 256U
#define UMS9117_MBOX_IRQ_LIMIT 256U
#define UMS9117_MBOX_OPEN 0xbeee0104U
#define UMS9117_MBOX_CMD 0x00010504U
#define UMS9117_MBOX_DONE 0x00020604U
#define UMS9117_MBOX_EVENT_DATA 0x00010404U
#define UMS9117_MBOX_EVENT_SPACE 0x00020404U

#define UMS9117_SIPC_PHYS 0x80000000U
#define UMS9117_SIPC_BYTES 0x20000U
#define UMS9117_SBUF_DESCRIPTOR 0x04U
#define UMS9117_SBUF_DATA 0x6cU
#define UMS9117_SBUF_END 0xd86cU
#define UMS9117_SBUF_RING_BYTES 0x2400U
#define UMS9117_SBUF_RING_COUNT 3U
#define UMS9117_SBUF_RING_WORDS 8U

enum sbuf_event {
	UMS9117_SBUF_DATA_READY,
	UMS9117_SBUF_SPACE_READY,
	UMS9117_SBUF_EVENT_COUNT,
};

enum sbuf_word {
	UMS9117_SBUF_TX_ADDRESS,
	UMS9117_SBUF_TX_SIZE,
	UMS9117_SBUF_TX_READ,
	UMS9117_SBUF_TX_WRITE,
	UMS9117_SBUF_RX_ADDRESS,
	UMS9117_SBUF_RX_SIZE,
	UMS9117_SBUF_RX_READ,
	UMS9117_SBUF_RX_WRITE,
};

enum sbuf_counter_mode {
	UMS9117_SBUF_COUNTERS_WITNESS,
	UMS9117_SBUF_COUNTERS_STREAM,
};

struct mailbox_bank {
	void __iomem *base;
	int virq;
	u32 count;
	bool requested;
	bool enabled;
};

struct mailbox_message {
	u32 low;
	u32 high;
	u32 id;
	bool from_irq;
	bool local_open_at_receive;
};

struct mailbox_send {
	u32 low;
	u32 high;
	bool triggered;
	bool delivered;
};

struct sbuf_snapshot {
	u32 allocator;
	u32 ring_count;
	u32 rings[UMS9117_SBUF_RING_COUNT][UMS9117_SBUF_RING_WORDS];
};

struct sbuf_stream {
	bool enabled;
	bool attached;
	/* One delivery in flight, plus one pending DATA and one pending SPACE. */
	u32 pending;
	u32 active_event;
	struct mailbox_send notify;
};

/* One controller, one cold start; bind/unbind and CM4 restart are unsupported. */
static struct {
	void __iomem *sipc;
	struct device *dev;
	struct mailbox_bank send;
	struct mailbox_bank receive;
	struct mailbox_message queue[UMS9117_MBOX_EVENT_LIMIT];
	u32 queued;
	u32 consumed;
	u32 depth;
	u32 allocator_before;
	bool peer_open;
	bool peer_cmd;
	bool gate_enabled;
	bool prepared;
	bool stopped;
	bool suspended;
	bool predone_valid;
	bool continuous;
	struct mailbox_send open;
	struct mailbox_send done;
	struct sbuf_stream h4;
	int error;
} mailbox;

static void mailbox_mask_banks(void)
{
	if (!mailbox.gate_enabled)
		return;
	if (mailbox.send.base)
		writel(UMS9117_MBOX_MASK_ALL,
		       mailbox.send.base + UMS9117_MBOX_IRQ_MSK);
	if (mailbox.receive.base)
		writel(UMS9117_MBOX_MASK_ALL,
		       mailbox.receive.base + UMS9117_MBOX_IRQ_MSK);
	/* Complete peripheral masking before disabling Linux IRQs or resetting CM4. */
	mb();
}

static void mailbox_disable_irqs(void)
{
	mailbox_mask_banks();
	if (mailbox.send.requested && mailbox.send.enabled) {
		disable_irq_nosync(mailbox.send.virq);
		mailbox.send.enabled = false;
	}
	if (mailbox.receive.requested && mailbox.receive.enabled) {
		disable_irq_nosync(mailbox.receive.virq);
		mailbox.receive.enabled = false;
	}
}

/* Called with local IRQs excluded, or before either IRQ has been enabled. */
static int mailbox_fail(const char *reason, int error)
{
	if (!mailbox.error) {
		mailbox.error = error;
		dev_err(mailbox.dev, "mailbox %s: %d\n", reason, error);
	}
	if (mailbox.continuous)
		mailbox_disable_irqs();
	else
		mailbox_mask_banks();
	return mailbox.error;
}

static int mailbox_fifo_count(u32 status)
{
	u32 read_pointer = status >> 24;
	u32 write_pointer = (status >> 16) & 0xff;
	u32 count;

	if (read_pointer >= mailbox.depth || write_pointer >= mailbox.depth)
		return -EOVERFLOW;
	if (read_pointer == write_pointer)
		count = status & BIT(2) ? mailbox.depth : 0;
	else if (write_pointer > read_pointer)
		count = write_pointer - read_pointer;
	else
		count = mailbox.depth - read_pointer + write_pointer;
	return count <= mailbox.depth ? count : -EOVERFLOW;
}

/* The ring counters own progress; no reply is needed for these notifications. */
static bool sbuf_accept_event(const struct mailbox_message *message)
{
	return mailbox.h4.enabled &&
	       (message->low == UMS9117_MBOX_EVENT_DATA ||
		message->low == UMS9117_MBOX_EVENT_SPACE) &&
	       message->high < UMS9117_SBUF_RING_COUNT;
}

static int mailbox_accept_service_message(const struct mailbox_message *message)
{
	if ((message->id & 7) != 1 ||
	    ((message->low & 0xff) != 4 && (message->low & 0xff) != 5))
		return mailbox_fail("unexpected route", -EPROTO);
	/* Channel 5 carries controller logging, with no AP consumer. */
	if ((message->low & 0xff) == 5)
		return 0;
	if (message->from_irq && sbuf_accept_event(message))
		return 0;
	return mailbox_fail("unexpected protocol", -EPROTO);
}

/* Only preparation while masked, then the receive hard IRQ, may pop entries. */
static int mailbox_drain_receive(bool from_irq)
{
	u32 batches = mailbox.continuous ? 1 : UMS9117_MBOX_EVENT_LIMIT;
	u32 batch;

	for (batch = 0; batch < batches; batch++) {
		u32 status =
			readl(mailbox.receive.base + UMS9117_MBOX_FIFO_STS);
		u32 irq_status =
			readl(mailbox.receive.base + UMS9117_MBOX_IRQ_STS);
		u32 sources = status & UMS9117_MBOX_SOURCE_MASK;
		int count = mailbox_fifo_count(status);
		int i;

		if (irq_status & UMS9117_MBOX_FIFO_UNAVAILABLE)
			return mailbox_fail("FIFO_UNAVAILABLE", -EIO);
		if (count < 0)
			return mailbox_fail("FIFO_OR_QUEUE_BOUND", count);
		for (i = 0; i < count; i++) {
			struct mailbox_message received;
			struct mailbox_message *message;

			if (mailbox.continuous) {
				message = &received;
			} else {
				if (mailbox.queued == UMS9117_MBOX_EVENT_LIMIT)
					return mailbox_fail(
						"FIFO_OR_QUEUE_BOUND",
						-EOVERFLOW);
				message = &mailbox.queue[mailbox.queued];
			}
			message->low = readl(mailbox.receive.base +
					     UMS9117_MBOX_MSG_L);
			message->high = readl(mailbox.receive.base +
					      UMS9117_MBOX_MSG_H);
			message->id =
				readl(mailbox.receive.base + UMS9117_MBOX_ID);
			message->from_irq = from_irq;
			message->local_open_at_receive = mailbox.open.triggered;
			writel(1, mailbox.receive.base + UMS9117_MBOX_TRI);
			if (mailbox.continuous) {
				if (mailbox_accept_service_message(message))
					return mailbox.error;
			} else {
				mailbox.queued++;
			}
		}
		writel(sources | BIT(0),
		       mailbox.receive.base + UMS9117_MBOX_IRQ_STS);
		status = readl(mailbox.receive.base + UMS9117_MBOX_FIFO_STS);
		count = mailbox_fifo_count(status);
		if (count < 0)
			return mailbox_fail("FIFO_OR_QUEUE_BOUND", count);
		if (!count && !(status & UMS9117_MBOX_SOURCE_MASK))
			return 0;
	}
	if (mailbox.continuous) {
		/*
		 * One FIFO snapshot (at most depth entries) per runtime IRQ.
		 * FIFO-not-empty is level-triggered: raced arrivals remain in the
		 * hardware FIFO for the next IRQ after this batch's W1C completes.
		 */
		mb();
		return 0;
	}
	return mailbox_fail("FIFO_OR_QUEUE_BOUND", -EOVERFLOW);
}

static irqreturn_t mailbox_receive_irq(int irq, void *data)
{
	mailbox.receive.count++;
	if (mailbox.error || mailbox.stopped || mailbox.suspended) {
		mailbox_mask_banks();
		return IRQ_HANDLED;
	}
	if (!mailbox.continuous &&
	    mailbox.receive.count > UMS9117_MBOX_IRQ_LIMIT)
		mailbox_fail("IRQ_BOUND", -EOVERFLOW);
	else
		mailbox_drain_receive(true);
	return IRQ_HANDLED;
}

static irqreturn_t mailbox_send_irq(int irq, void *data)
{
	struct mailbox_send *send;
	u32 status;
	u32 clear;
	u32 irq_status;

	mailbox.send.count++;
	if (mailbox.error || mailbox.stopped || mailbox.suspended) {
		mailbox_mask_banks();
		return IRQ_HANDLED;
	}
	status = readl(mailbox.send.base + UMS9117_MBOX_FIFO_STS);
	irq_status = readl(mailbox.send.base + UMS9117_MBOX_IRQ_STS);
	if (!mailbox.continuous &&
	    mailbox.send.count > UMS9117_MBOX_IRQ_LIMIT) {
		mailbox_fail("IRQ_BOUND", -EOVERFLOW);
		return IRQ_HANDLED;
	}
	clear = status & UMS9117_MBOX_CLEAR_MASK;
	if ((irq_status & UMS9117_MBOX_FIFO_UNAVAILABLE) ||
	    clear != UMS9117_MBOX_PEER_DELIVERED ||
	    (status &
	     ~(UMS9117_MBOX_PEER_DELIVERED | UMS9117_MBOX_PEER_PENDING))) {
		mailbox_fail("UNEXPECTED_SEND_IRQ", -EPROTO);
		return IRQ_HANDLED;
	}
	if (mailbox.open.triggered && !mailbox.open.delivered) {
		send = &mailbox.open;
	} else if (mailbox.done.triggered && !mailbox.done.delivered) {
		send = &mailbox.done;
	} else if (mailbox.h4.enabled && mailbox.h4.notify.triggered &&
		   !mailbox.h4.notify.delivered) {
		send = &mailbox.h4.notify;
	} else {
		mailbox_fail("UNEXPECTED_SEND_IRQ", -EPROTO);
		return IRQ_HANDLED;
	}
	writel(clear, mailbox.send.base + UMS9117_MBOX_FIFO_RST);
	writel(BIT(0), mailbox.send.base + UMS9117_MBOX_IRQ_STS);
	send->delivered = true;
	writel(UMS9117_MBOX_SEND_IDLE,
	       mailbox.send.base + UMS9117_MBOX_IRQ_MSK);
	/* Complete the W1C sequence before returning to the level-triggered GIC. */
	mb();
	return IRQ_HANDLED;
}

static int mailbox_claim_bank(struct platform_device *pdev, const char *name,
			      resource_size_t start, struct mailbox_bank *bank)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource->start != start ||
	    resource_size(resource) != 0x1000)
		return -EINVAL;
	bank->base = devm_ioremap_resource(&pdev->dev, resource);
	return PTR_ERR_OR_ZERO(bank->base);
}

static int mailbox_claim_irq(struct platform_device *pdev, const char *name,
			     u32 expected, irq_handler_t handler,
			     struct mailbox_bank *bank)
{
	struct irq_data *data;
	int ret;

	bank->virq = platform_get_irq_byname(pdev, name);
	if (bank->virq < 0)
		return bank->virq;
	data = irq_get_irq_data(bank->virq);
	if (!data || irqd_to_hwirq(data) != expected ||
	    irq_get_trigger_type(bank->virq) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;
	ret = devm_request_irq(&pdev->dev, bank->virq, handler,
			       IRQF_NO_AUTOEN | IRQF_NO_THREAD, name, bank);
	if (!ret)
		bank->requested = true;
	return ret;
}

int ums9117_cm4_mailbox_init(struct platform_device *pdev)
{
	int ret;

	mailbox.dev = &pdev->dev;
	ret = mailbox_claim_bank(pdev, "send", 0x400a0000, &mailbox.send);
	if (!ret)
		ret = mailbox_claim_bank(pdev, "receive", 0x400a8000,
					 &mailbox.receive);
	if (!ret)
		ret = mailbox_claim_irq(pdev, "send", 100, mailbox_send_irq,
					&mailbox.send);
	if (!ret)
		ret = mailbox_claim_irq(pdev, "receive", 101,
					mailbox_receive_irq, &mailbox.receive);
	return ret;
}

static bool sbuf_layout_valid(const struct sbuf_snapshot *snapshot,
			      enum sbuf_counter_mode mode)
{
	u32 next_buffer = UMS9117_SIPC_PHYS + UMS9117_SBUF_DATA;
	u32 ring;

	if (snapshot->ring_count != UMS9117_SBUF_RING_COUNT)
		return false;
	for (ring = 0; ring < UMS9117_SBUF_RING_COUNT; ring++) {
		const u32 *words = snapshot->rings[ring];
		u32 direction;

		for (direction = 0; direction < 2; direction++) {
			u32 address = words[direction * 4];
			u32 size = words[direction * 4 + 1];
			u32 rd = words[direction * 4 + 2];
			u32 wr = words[direction * 4 + 3];
			bool counters_valid;

			if (mode == UMS9117_SBUF_COUNTERS_STREAM)
				counters_valid = (u32)(wr - rd) <= size;
			else if (!ring && !direction)
				counters_valid = !wr && rd == U32_MAX;
			else
				counters_valid = !rd && !wr;

			if ((address & 3) || address != next_buffer ||
			    size != UMS9117_SBUF_RING_BYTES ||
			    address > UMS9117_SIPC_PHYS + UMS9117_SIPC_BYTES -
					      size ||
			    !counters_valid)
				return false;
			next_buffer = address + size;
		}
	}
	return next_buffer == UMS9117_SIPC_PHYS + UMS9117_SBUF_END;
}

static bool sbuf_read_snapshot(struct sbuf_snapshot *snapshot,
			       enum sbuf_counter_mode mode)
{
	u32 ring;
	u32 word;

	snapshot->allocator = readl(mailbox.sipc);
	snapshot->ring_count = readl(mailbox.sipc + UMS9117_SBUF_DESCRIPTOR);
	for (ring = 0; ring < UMS9117_SBUF_RING_COUNT; ring++)
		for (word = 0; word < UMS9117_SBUF_RING_WORDS; word++)
			snapshot->rings[ring][word] =
				readl(mailbox.sipc + 8 + ring * 32 + word * 4);
	/* Observe peer publication before reading data or reusing space. */
	mb();
	return snapshot->allocator == mailbox.allocator_before &&
	       sbuf_layout_valid(snapshot, mode);
}

static int sbuf_prepare(void)
{
	u32 ring;
	u32 address = UMS9117_SIPC_PHYS + UMS9117_SBUF_DATA;
	struct sbuf_snapshot snapshot;

	mailbox.allocator_before = readl(mailbox.sipc);
	memset_io(mailbox.sipc + UMS9117_SBUF_DESCRIPTOR, 0,
		  UMS9117_SBUF_END - UMS9117_SBUF_DESCRIPTOR);
	writel(UMS9117_SBUF_RING_COUNT, mailbox.sipc + UMS9117_SBUF_DESCRIPTOR);
	for (ring = 0; ring < UMS9117_SBUF_RING_COUNT; ring++) {
		void __iomem *descriptor = mailbox.sipc + 8 + ring * 32;

		writel(address, descriptor);
		writel(UMS9117_SBUF_RING_BYTES, descriptor + 4);
		address += UMS9117_SBUF_RING_BYTES;
		writel(address, descriptor + 16);
		writel(UMS9117_SBUF_RING_BYTES, descriptor + 20);
		address += UMS9117_SBUF_RING_BYTES;
	}
	/*
	 * The peer first sets tx_rd = tx_wr during attach, before any ring read.
	 * This valid one-byte pending state makes that destructive store visible;
	 * no H4 bytes or DATA event are published until it has become empty.
	 */
	writel(U32_MAX, mailbox.sipc + 8 + UMS9117_SBUF_TX_READ * 4);
	/* Publish every descriptor, counter and buffer before validating readback. */
	mb();
	if (!sbuf_read_snapshot(&snapshot, UMS9117_SBUF_COUNTERS_WITNESS))
		return mailbox_fail("SBUF_LAYOUT_CHANGED", -EUCLEAN);
	return 0;
}

int ums9117_cm4_mailbox_prepare(void __iomem *sipc, struct regmap *aon)
{
	unsigned long flags;
	u32 send_fifo, receive_fifo;
	u32 send_irq, receive_irq;
	u32 depth, version;
	int ret;

	mailbox.sipc = sipc;
	mailbox.open.low = UMS9117_MBOX_OPEN;
	mailbox.done.low = UMS9117_MBOX_DONE;
	mailbox.done.high = UMS9117_SIPC_PHYS + UMS9117_SBUF_DESCRIPTOR;
	ret = regmap_write(aon, 0x1004, BIT(21));
	if (ret)
		return ret;
	mailbox.gate_enabled = true;
	mailbox_mask_banks();
	version = readl(mailbox.receive.base + UMS9117_MBOX_VERSION);
	depth = readl(mailbox.receive.base + UMS9117_MBOX_FIFO_DEPTH);
	if (depth >= 128 || version == U32_MAX)
		return mailbox_fail("BAD_FIFO_DEPTH_VERSION", -EINVAL);
	mailbox.depth = depth + 1;
	writel(0, mailbox.receive.base + UMS9117_MBOX_FIFO_RST);
	send_fifo = readl(mailbox.send.base + UMS9117_MBOX_FIFO_STS);
	receive_fifo = readl(mailbox.receive.base + UMS9117_MBOX_FIFO_STS);
	send_irq = readl(mailbox.send.base + UMS9117_MBOX_IRQ_STS);
	receive_irq = readl(mailbox.receive.base + UMS9117_MBOX_IRQ_STS);
	if ((send_irq | receive_irq) & UMS9117_MBOX_FIFO_UNAVAILABLE)
		return mailbox_fail("FIFO_UNAVAILABLE", -EIO);
	ret = mailbox_drain_receive(false);
	if (ret)
		return ret;
	if (mailbox.queued || (receive_fifo & UMS9117_MBOX_SOURCE_MASK) ||
	    send_fifo || readl(mailbox.send.base + UMS9117_MBOX_FIFO_STS))
		return mailbox_fail("STALE_MAILBOX_STATE", -EBUSY);
	ret = sbuf_prepare();
	if (ret)
		return ret;
	mailbox.prepared = true;
	mailbox.send.enabled = true;
	enable_irq(mailbox.send.virq);
	mailbox.receive.enabled = true;
	enable_irq(mailbox.receive.virq);
	local_irq_save(flags);
	if (!mailbox.error) {
		writel(UMS9117_MBOX_SEND_IDLE,
		       mailbox.send.base + UMS9117_MBOX_IRQ_MSK);
		writel(UMS9117_MBOX_RECEIVE_ARMED,
		       mailbox.receive.base + UMS9117_MBOX_IRQ_MSK);
		/* Handlers and peripheral masks must precede CM4 reset release. */
		mb();
	}
	local_irq_restore(flags);
	return mailbox.error;
}

static void mailbox_accept_message(u32 index)
{
	const struct mailbox_message *message = &mailbox.queue[index];
	const char *reason = "UNEXPECTED_PROTOCOL";

	if ((message->id & 7) != 1 ||
	    ((message->low & 0xff) != 4 && (message->low & 0xff) != 5)) {
		reason = "UNEXPECTED_ROUTE";
		goto fail;
	}
	if ((message->low & 0xff) == 5)
		return;
	if (!message->from_irq)
		goto fail;
	if (sbuf_accept_event(message))
		return;
	if (message->low == UMS9117_MBOX_OPEN && !message->high &&
	    !mailbox.peer_open) {
		mailbox.peer_open = true;
		return;
	}
	if (message->low == UMS9117_MBOX_CMD && !message->high &&
	    mailbox.peer_open && message->local_open_at_receive &&
	    !mailbox.peer_cmd) {
		mailbox.peer_cmd = true;
		return;
	}
fail:
	mailbox_fail(reason, -EPROTO);
}

/* The startup queue serializes IRQ reception with the handshake. */
static void mailbox_consume_queue(void)
{
	unsigned long flags;

	local_irq_save(flags);
	while (mailbox.consumed < mailbox.queued && !mailbox.error)
		mailbox_accept_message(mailbox.consumed++);
	local_irq_restore(flags);
}

static int mailbox_trigger(struct mailbox_send *send)
{
	unsigned long flags;
	u32 status;
	int ret = -EINPROGRESS;

	local_irq_save(flags);
	if (mailbox.error || mailbox.stopped ||
	    mailbox.consumed != mailbox.queued)
		goto out;
	status = readl(mailbox.send.base + UMS9117_MBOX_FIFO_STS);
	if (status & UMS9117_MBOX_BLOCK_MASK) {
		mailbox_fail("SEND_BLOCKED", -EBUSY);
		goto out;
	}
	if (status & ~UMS9117_MBOX_PEER_PENDING) {
		mailbox_fail("STALE_SEND_STATE", -EPROTO);
		goto out;
	}
	if (status & UMS9117_MBOX_PEER_PENDING)
		goto out;
	writel(UMS9117_MBOX_SEND_DELIVERY,
	       mailbox.send.base + UMS9117_MBOX_IRQ_MSK);
	writel(send->low, mailbox.send.base + UMS9117_MBOX_MSG_L);
	writel(send->high, mailbox.send.base + UMS9117_MBOX_MSG_H);
	writel(1, mailbox.send.base + UMS9117_MBOX_ID);
	send->triggered = true;
	/* IRQ handlers must see the committed state before the peer can answer. */
	mb();
	writel(1, mailbox.send.base + UMS9117_MBOX_TRI);
	/* Complete the trigger before allowing a reciprocal IRQ to run. */
	mb();
out:
	if (mailbox.error)
		ret = mailbox.error;
	local_irq_restore(flags);
	return ret;
}

static int sbuf_poll_notifications(void)
{
	struct sbuf_stream *stream = &mailbox.h4;
	unsigned long flags;
	bool pending;

	local_irq_save(flags);
	if (stream->notify.triggered && !stream->notify.delivered) {
		local_irq_restore(flags);
		return -EINPROGRESS;
	}
	if ((!stream->notify.low || stream->notify.delivered) &&
	    stream->pending) {
		stream->active_event = __ffs(stream->pending);
		stream->pending &= ~BIT(stream->active_event);
		memset(&stream->notify, 0, sizeof(stream->notify));
		stream->notify.low = stream->active_event ==
						     UMS9117_SBUF_DATA_READY ?
					     UMS9117_MBOX_EVENT_DATA :
					     UMS9117_MBOX_EVENT_SPACE;
	}
	pending = stream->notify.low && !stream->notify.triggered;
	local_irq_restore(flags);
	if (pending)
		return mailbox_trigger(&stream->notify);
	return 0;
}

int ums9117_cm4_mailbox_poll(void)
{
	unsigned long flags;
	struct sbuf_snapshot snapshot;
	int ret;

	if (mailbox.error)
		return mailbox.error;
	if (!mailbox.prepared)
		return -EINVAL;
	if (mailbox.suspended)
		return -EHOSTDOWN;
	if (mailbox.stopped)
		return mailbox.done.delivered ? 0 : -EINPROGRESS;
	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	mailbox_consume_queue();
	if (mailbox.error)
		return mailbox.error;
	if (mailbox.done.delivered) {
		if (mailbox.h4.enabled)
			return sbuf_poll_notifications();
		return 0;
	}
	if (mailbox.peer_open && !mailbox.open.triggered)
		return mailbox_trigger(&mailbox.open);
	if (!mailbox.open.delivered || !mailbox.peer_cmd)
		return -EINPROGRESS;
	if (mailbox.done.triggered)
		return -EINPROGRESS;
	/* CMD can precede the send ISR for OPEN. Its receive-time state was saved. */
	if (!mailbox.predone_valid) {
		ret = sbuf_read_snapshot(&snapshot,
					 UMS9117_SBUF_COUNTERS_WITNESS) ?
			      0 :
			      -EUCLEAN;
		if (ret) {
			local_irq_save(flags);
			ret = mailbox_fail("SBUF_LAYOUT_CHANGED", ret);
			local_irq_restore(flags);
			return ret;
		}
		mailbox.predone_valid = true;
	}
	/* The validated descriptor is visible before publishing DONE to the peer. */
	mb();
	return mailbox_trigger(&mailbox.done);
}

/* Local IRQs excluded; the calling loop is the sole AP producer/consumer. */
static int sbuf_read_live_ring(u32 *ring)
{
	struct sbuf_snapshot snapshot;

	if (!sbuf_read_snapshot(&snapshot, UMS9117_SBUF_COUNTERS_STREAM))
		return mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
	memcpy(ring, snapshot.rings[0], sizeof(snapshot.rings[0]));
	return 0;
}

int ums9117_cm4_mailbox_h4_begin(void)
{
	u32 ring[UMS9117_SBUF_RING_WORDS];
	unsigned long flags;
	int ret;

	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	local_irq_save(flags);
	if (mailbox.error) {
		ret = mailbox.error;
		goto out;
	}
	if (!mailbox.prepared || mailbox.stopped) {
		ret = -EINVAL;
		goto out;
	}
	if (mailbox.h4.enabled) {
		ret = 0;
		goto out;
	}
	ret = -EINPROGRESS;
	if (!mailbox.done.delivered)
		goto out;
	ret = sbuf_read_live_ring(ring);
	if (ret)
		goto out;
	if (ring[UMS9117_SBUF_TX_WRITE] ||
	    (ring[UMS9117_SBUF_TX_READ] &&
	     ring[UMS9117_SBUF_TX_READ] != U32_MAX)) {
		ret = mailbox_fail("SBUF_ATTACH_COUNTER", -EPROTO);
		goto out;
	}
	if (ring[UMS9117_SBUF_TX_READ] == U32_MAX) {
		ret = -EINPROGRESS;
		goto out;
	}
	mailbox.h4.attached = true;
	mailbox.h4.enabled = true;
out:
	local_irq_restore(flags);
	return ret;
}

int ums9117_cm4_mailbox_h4_continue(void)
{
	unsigned long flags;
	int ret;

	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	local_irq_save(flags);
	ret = mailbox.error;
	if (ret)
		goto out;
	if (mailbox.continuous) {
		ret = -EALREADY;
		goto out;
	}
	if (!mailbox.prepared || mailbox.stopped || !mailbox.done.delivered ||
	    !mailbox.h4.enabled || !mailbox.h4.attached) {
		ret = -EINVAL;
		goto out;
	}
	/* Classify the startup queue before switching IRQs to continuous service. */
	mailbox_consume_queue();
	ret = mailbox.error;
	if (ret)
		goto out;
	mailbox.continuous = true;
out:
	local_irq_restore(flags);
	return ret;
}

static void sbuf_queue_event(enum sbuf_event event)
{
	struct sbuf_stream *stream = &mailbox.h4;

	stream->pending |= BIT(event);
}

static int sbuf_stream_ready(void)
{
	if (mailbox.error)
		return mailbox.error;
	if (!mailbox.h4.enabled || mailbox.stopped)
		return -ESHUTDOWN;
	if (mailbox.suspended)
		return -EHOSTDOWN;
	mailbox_consume_queue();
	return mailbox.error;
}

int ums9117_cm4_mailbox_h4_write(const u8 *data, size_t bytes, size_t *written)
{
	void __iomem *buffer;
	u32 ring[UMS9117_SBUF_RING_WORDS];
	u32 occupied;
	u32 count;
	u32 offset;
	u32 first;
	u32 wr;
	u32 rd;
	unsigned long flags;
	int ret;

	if (!written || (!data && bytes))
		return -EINVAL;
	*written = 0;
	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	local_irq_save(flags);
	ret = sbuf_stream_ready();
	if (ret || !bytes)
		goto out;
	ret = sbuf_read_live_ring(ring);
	if (ret)
		goto out;
	wr = ring[UMS9117_SBUF_TX_WRITE];
	occupied = wr - ring[UMS9117_SBUF_TX_READ];
	count = min_t(size_t, bytes, UMS9117_SBUF_RING_BYTES - occupied);
	if (!count)
		goto out;
	offset = wr % UMS9117_SBUF_RING_BYTES;
	first = min(count, UMS9117_SBUF_RING_BYTES - offset);
	buffer = mailbox.sipc + ring[UMS9117_SBUF_TX_ADDRESS] -
		 UMS9117_SIPC_PHYS;
	memcpy_toio(buffer + offset, data, first);
	if (count > first)
		memcpy_toio(buffer, data + first, count - first);
	/* Publish completed payload before tx_wr, then sample the peer consumer. */
	mb();
	writel(wr + count, mailbox.sipc + 8 + UMS9117_SBUF_TX_WRITE * 4);
	/* Complete the index publication before deciding whether to wake the peer. */
	mb();
	*written = count;
	rd = readl(mailbox.sipc + 8 + UMS9117_SBUF_TX_READ * 4);
	if ((u32)(wr + count - rd) > UMS9117_SBUF_RING_BYTES) {
		ret = mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
		goto out;
	}
	/* Recheck after publication: the peer may have drained while we copied. */
	if (rd == wr)
		sbuf_queue_event(UMS9117_SBUF_DATA_READY);
out:
	local_irq_restore(flags);
	return ret;
}

int ums9117_cm4_mailbox_h4_read(u8 *data, size_t capacity, size_t *received)
{
	void __iomem *buffer;
	u32 ring[UMS9117_SBUF_RING_WORDS];
	u32 occupied;
	u32 count;
	u32 offset;
	u32 first;
	u32 rd;
	u32 wr;
	unsigned long flags;
	int ret;

	if (!received || (!data && capacity))
		return -EINVAL;
	*received = 0;
	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	local_irq_save(flags);
	ret = sbuf_stream_ready();
	if (ret || !capacity)
		goto out;
	ret = sbuf_read_live_ring(ring);
	if (ret)
		goto out;
	rd = ring[UMS9117_SBUF_RX_READ];
	occupied = ring[UMS9117_SBUF_RX_WRITE] - rd;
	count = min_t(size_t, capacity, occupied);
	if (!count)
		goto out;
	offset = rd % UMS9117_SBUF_RING_BYTES;
	first = min(count, UMS9117_SBUF_RING_BYTES - offset);
	buffer = mailbox.sipc + ring[UMS9117_SBUF_RX_ADDRESS] -
		 UMS9117_SIPC_PHYS;
	memcpy_fromio(data, buffer + offset, first);
	if (count > first)
		memcpy_fromio(data + first, buffer, count - first);
	/* Finish copying before releasing this space to the peer producer. */
	mb();
	writel(rd + count, mailbox.sipc + 8 + UMS9117_SBUF_RX_READ * 4);
	/* Complete the space release before checking the producer for a wake. */
	mb();
	*received = count;
	wr = readl(mailbox.sipc + 8 + UMS9117_SBUF_RX_WRITE * 4);
	if ((u32)(wr - rd - count) > UMS9117_SBUF_RING_BYTES) {
		ret = mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
		goto out;
	}
	/* The producer can reach full while this copy is in progress. */
	if ((u32)(wr - rd) == UMS9117_SBUF_RING_BYTES)
		sbuf_queue_event(UMS9117_SBUF_SPACE_READY);
out:
	local_irq_restore(flags);
	return ret;
}

int ums9117_cm4_mailbox_suspend(void)
{
	u32 ring[UMS9117_SBUF_RING_WORDS];
	u32 status;
	unsigned long flags;
	int ret;

	if (irqs_disabled())
		return -EINVAL;
	local_irq_save(flags);
	ret = mailbox.error;
	if (ret || mailbox.suspended)
		goto out;
	if (!mailbox.continuous || mailbox.stopped) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = sbuf_read_live_ring(ring);
	if (ret)
		goto out;
	ret = -EBUSY;
	if (ring[UMS9117_SBUF_TX_WRITE] != ring[UMS9117_SBUF_TX_READ] ||
	    ring[UMS9117_SBUF_RX_WRITE] != ring[UMS9117_SBUF_RX_READ] ||
	    mailbox.h4.pending ||
	    (mailbox.h4.notify.low && !mailbox.h4.notify.delivered))
		goto out;
	status = readl(mailbox.receive.base + UMS9117_MBOX_FIFO_STS);
	if (mailbox_fifo_count(status) || (status & UMS9117_MBOX_SOURCE_MASK) ||
	    readl(mailbox.send.base + UMS9117_MBOX_FIFO_STS))
		goto out;
	/* No FIFO entry is popped here: normal hard IRQs own delivery. */
	mailbox_disable_irqs();
	mailbox.suspended = true;
	ret = 0;
out:
	local_irq_restore(flags);
	if (!ret && mailbox.suspended) {
		synchronize_irq(mailbox.send.virq);
		synchronize_irq(mailbox.receive.virq);
	}
	return ret;
}

int ums9117_cm4_mailbox_resume(void)
{
	u32 ring[UMS9117_SBUF_RING_WORDS];
	unsigned long flags;
	int ret;

	if (irqs_disabled())
		return -EINVAL;
	local_irq_save(flags);
	ret = mailbox.error;
	if (ret || !mailbox.suspended)
		goto out;
	if (mailbox.stopped) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = sbuf_read_live_ring(ring);
	if (ret)
		goto out;
	mailbox.suspended = false;
	local_irq_restore(flags);

	/* Retained arrivals stay in the FIFO until its original mask is restored. */
	mailbox.send.enabled = true;
	enable_irq(mailbox.send.virq);
	mailbox.receive.enabled = true;
	enable_irq(mailbox.receive.virq);
	local_irq_save(flags);
	if (!mailbox.error) {
		writel(UMS9117_MBOX_SEND_IDLE,
		       mailbox.send.base + UMS9117_MBOX_IRQ_MSK);
		writel(UMS9117_MBOX_RECEIVE_ARMED,
		       mailbox.receive.base + UMS9117_MBOX_IRQ_MSK);
		/* Restore device masks before local IRQs can service retained entries. */
		mb();
	}
	ret = mailbox.error;
out:
	local_irq_restore(flags);
	return ret;
}

void ums9117_cm4_mailbox_stop(void)
{
	unsigned long flags;

	local_irq_save(flags);
	if (mailbox.stopped)
		goto out;
	mailbox_disable_irqs();
	/* Process the IRQ-queued tail without touching an unserviced hardware FIFO. */
	if (mailbox.prepared)
		mailbox_consume_queue();
	mailbox.stopped = true;
out:
	local_irq_restore(flags);
}
