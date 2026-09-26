// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 CM4 channel-4 handshake and Bluetooth/FM streams. */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/string.h>

#include "cm4-mailbox.h"

#define UMS9117_MBOX_EVENT_LIMIT 256U
#define UMS9117_MBOX_STOP_TIMEOUT_MS 2000U
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

struct mailbox_message {
	u32 low;
	u32 high;
	bool local_open_at_receive;
};

struct mailbox_send {
	u32 data[2];
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
	/* One delivery in flight; pending DATA/SPACE notifications per ring. */
	u32 pending;
	u32 active_event;
	struct mailbox_send notify;
};

/* One controller; the platform owner serializes starts and stops. */
static struct {
	void __iomem *sipc;
	struct device *dev;
	struct mbox_client client;
	struct mbox_chan *channel;
	struct mailbox_message queue[UMS9117_MBOX_EVENT_LIMIT];
	u32 queued;
	u32 consumed;
	u32 allocator_before;
	bool peer_open;
	bool peer_cmd;
	bool prepared;
	bool stopped;
	bool suspended;
	bool predone_valid;
	bool continuous;
	struct mailbox_send open;
	struct mailbox_send done;
	struct sbuf_stream stream;
	int error;
	int stop_error;
} mailbox;

/* Called with local IRQs excluded or from an atomic mailbox callback. */
static int mailbox_fail(const char *reason, int error)
{
	if (!mailbox.error) {
		mailbox.error = error;
		dev_err(mailbox.dev, "mailbox %s: %pe\n", reason,
			ERR_PTR(error));
	}
	return mailbox.error;
}

/* The ring counters own progress; no reply is needed for these notifications. */
static bool sbuf_accept_event(const struct mailbox_message *message)
{
	return mailbox.stream.enabled &&
	       (message->low == UMS9117_MBOX_EVENT_DATA ||
		message->low == UMS9117_MBOX_EVENT_SPACE) &&
	       message->high < UMS9117_SBUF_RING_COUNT;
}

static int mailbox_accept_service_message(const struct mailbox_message *message)
{
	if ((message->low & 0xff) != 4 && (message->low & 0xff) != 5)
		return mailbox_fail("unexpected route", -EPROTO);
	/* Channel 5 carries controller logging, with no AP consumer. */
	if ((message->low & 0xff) == 5)
		return 0;
	if (sbuf_accept_event(message))
		return 0;
	return mailbox_fail("unexpected protocol", -EPROTO);
}

/* The native controller owns the FIFO; its callback data dies on return. */
static void mailbox_receive(struct mbox_client *client, void *data)
{
	const u32 *payload = data;
	struct mailbox_message message = {
		.low = payload[0],
		.high = payload[1],
		.local_open_at_receive = mailbox.open.triggered,
	};

	if (mailbox.error || mailbox.stopped)
		return;
	if (mailbox.continuous) {
		mailbox_accept_service_message(&message);
		return;
	}
	if (mailbox.queued == UMS9117_MBOX_EVENT_LIMIT) {
		mailbox_fail("RX_QUEUE_BOUND", -EOVERFLOW);
		return;
	}
	mailbox.queue[mailbox.queued++] = message;
}

static void mailbox_transmitted(struct mbox_client *client, void *data,
				int result)
{
	struct mailbox_send *send;

	if (data == mailbox.open.data)
		send = &mailbox.open;
	else if (data == mailbox.done.data)
		send = &mailbox.done;
	else if (data == mailbox.stream.notify.data)
		send = &mailbox.stream.notify;
	else {
		mailbox_fail("UNEXPECTED_TX_DONE", -EPROTO);
		return;
	}
	if (result) {
		mailbox_fail("TX_FAILED", result);
		return;
	}
	if (!send->triggered || send->delivered) {
		mailbox_fail("UNEXPECTED_TX_DONE", -EPROTO);
		return;
	}
	send->delivered = true;
}

static void mailbox_release_channel(void *data)
{
	mbox_free_channel(mailbox.channel);
	mailbox.channel = NULL;
}

static int mailbox_request_channel(struct device *dev)
{
	int ret;

	mailbox.dev = dev;
	mailbox.client.dev = dev;
	mailbox.client.rx_callback = mailbox_receive;
	mailbox.client.tx_done = mailbox_transmitted;
	mailbox.channel = mbox_request_channel_byname(&mailbox.client, "cm4");
	if (IS_ERR(mailbox.channel)) {
		ret = PTR_ERR(mailbox.channel);
		mailbox.channel = NULL;
		return ret;
	}
	return 0;
}

int ums9117_cm4_mailbox_init(struct device *dev)
{
	int ret;

	ret = mailbox_request_channel(dev);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, mailbox_release_channel, NULL);
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

int ums9117_cm4_mailbox_prepare(void __iomem *sipc)
{
	struct device *dev = mailbox.dev;
	unsigned long flags;
	int ret;

	if (!dev || !sipc || irqs_disabled() ||
	    (mailbox.prepared && !mailbox.stopped))
		return -EINVAL;
	if (mailbox.stopped) {
		if (mailbox.stop_error)
			return mailbox.stop_error;
		/*
		 * CM4 is held in reset. Retire the old provider queue and reset
		 * its receive FIFO before accepting a new startup handshake.
		 */
		mailbox_release_channel(NULL);
		memset(&mailbox, 0, sizeof(mailbox));
		mailbox.stopped = true;
		ret = mailbox_request_channel(dev);
		if (ret)
			return ret;
		mailbox.stopped = false;
	}
	if (!mailbox.channel)
		return -EINVAL;
	mailbox.sipc = sipc;
	mailbox.open.data[0] = UMS9117_MBOX_OPEN;
	mailbox.done.data[0] = UMS9117_MBOX_DONE;
	mailbox.done.data[1] = UMS9117_SIPC_PHYS + UMS9117_SBUF_DESCRIPTOR;
	local_irq_save(flags);
	if (mailbox.error)
		ret = mailbox.error;
	else if (mailbox.queued)
		ret = mailbox_fail("STALE_MAILBOX_STATE", -EBUSY);
	else
		ret = 0;
	local_irq_restore(flags);
	if (ret)
		return ret;
	ret = sbuf_prepare();
	if (ret)
		return ret;
	local_irq_save(flags);
	if (mailbox.error)
		ret = mailbox.error;
	else if (mailbox.queued)
		ret = mailbox_fail("STALE_MAILBOX_STATE", -EBUSY);
	else
		mailbox.prepared = true;
	local_irq_restore(flags);
	return ret;
}

static void mailbox_accept_message(u32 index)
{
	const struct mailbox_message *message = &mailbox.queue[index];
	const char *reason = "UNEXPECTED_PROTOCOL";

	if ((message->low & 0xff) != 4 && (message->low & 0xff) != 5) {
		reason = "UNEXPECTED_ROUTE";
		goto fail;
	}
	if ((message->low & 0xff) == 5)
		return;
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
	int ret = -EINPROGRESS;

	local_irq_save(flags);
	if (mailbox.error || mailbox.stopped || mailbox.suspended ||
	    mailbox.consumed != mailbox.queued)
		goto out;
	send->triggered = true;
	ret = mbox_send_message(mailbox.channel, send->data);
	if (ret >= 0)
		ret = -EINPROGRESS;
	else {
		send->triggered = false;
		ret = mailbox_fail("TX_SUBMIT_FAILED", ret);
	}
out:
	if (mailbox.error)
		ret = mailbox.error;
	local_irq_restore(flags);
	return ret;
}

static int sbuf_poll_notifications(void)
{
	struct sbuf_stream *stream = &mailbox.stream;
	unsigned long flags;
	bool pending;

	local_irq_save(flags);
	if (stream->notify.triggered && !stream->notify.delivered) {
		local_irq_restore(flags);
		return -EINPROGRESS;
	}
	if ((!stream->notify.data[0] || stream->notify.delivered) &&
	    stream->pending) {
		stream->active_event = __ffs(stream->pending);
		stream->pending &= ~BIT(stream->active_event);
		memset(&stream->notify, 0, sizeof(stream->notify));
		stream->notify.data[0] =
			stream->active_event % UMS9117_SBUF_EVENT_COUNT ==
					UMS9117_SBUF_DATA_READY ?
				UMS9117_MBOX_EVENT_DATA :
				UMS9117_MBOX_EVENT_SPACE;
		stream->notify.data[1] =
			stream->active_event / UMS9117_SBUF_EVENT_COUNT;
	}
	pending = stream->notify.data[0] && !stream->notify.triggered;
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
		if (mailbox.stream.enabled)
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
static int sbuf_read_live_ring(u32 index, u32 *ring)
{
	struct sbuf_snapshot snapshot;

	if (!sbuf_read_snapshot(&snapshot, UMS9117_SBUF_COUNTERS_STREAM))
		return mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
	memcpy(ring, snapshot.rings[index], sizeof(snapshot.rings[index]));
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
	if (mailbox.stream.enabled) {
		ret = 0;
		goto out;
	}
	ret = -EINPROGRESS;
	if (!mailbox.done.delivered)
		goto out;
	ret = sbuf_read_live_ring(0, ring);
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
	mailbox.stream.attached = true;
	mailbox.stream.enabled = true;
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
	    !mailbox.stream.enabled || !mailbox.stream.attached) {
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

static void sbuf_queue_event(u32 index, enum sbuf_event event)
{
	struct sbuf_stream *stream = &mailbox.stream;

	stream->pending |= BIT(index * UMS9117_SBUF_EVENT_COUNT + event);
}

static int sbuf_stream_ready(void)
{
	if (mailbox.error)
		return mailbox.error;
	if (!mailbox.stream.enabled || mailbox.stopped)
		return -ESHUTDOWN;
	if (mailbox.suspended)
		return -EHOSTDOWN;
	mailbox_consume_queue();
	return mailbox.error;
}

static int sbuf_write(u32 index, const u8 *data, size_t bytes, size_t *written,
		      bool whole_command)
{
	void __iomem *buffer;
	void __iomem *descriptor;
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
	if (whole_command && bytes > UMS9117_SBUF_RING_BYTES)
		return -EMSGSIZE;
	if (irqs_disabled())
		return mailbox_fail("AP_IRQS_DISABLED", -EINVAL);
	local_irq_save(flags);
	ret = sbuf_stream_ready();
	if (ret || !bytes)
		goto out;
	ret = sbuf_read_live_ring(index, ring);
	if (ret)
		goto out;
	wr = ring[UMS9117_SBUF_TX_WRITE];
	occupied = wr - ring[UMS9117_SBUF_TX_READ];
	/* FM consumes one command per read; do not fragment or coalesce commands. */
	if (whole_command && occupied) {
		ret = -EAGAIN;
		goto out;
	}
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
	descriptor = mailbox.sipc + 8 + index * 32;
	writel(wr + count, descriptor + UMS9117_SBUF_TX_WRITE * 4);
	/* Complete the index publication before deciding whether to wake the peer. */
	mb();
	*written = count;
	rd = readl(descriptor + UMS9117_SBUF_TX_READ * 4);
	if ((u32)(wr + count - rd) > UMS9117_SBUF_RING_BYTES) {
		ret = mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
		goto out;
	}
	/* Recheck after publication: the peer may have drained while we copied. */
	if (rd == wr)
		sbuf_queue_event(index, UMS9117_SBUF_DATA_READY);
out:
	local_irq_restore(flags);
	return ret;
}

static int sbuf_read(u32 index, u8 *data, size_t capacity, size_t *received)
{
	void __iomem *buffer;
	void __iomem *descriptor;
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
	ret = sbuf_read_live_ring(index, ring);
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
	descriptor = mailbox.sipc + 8 + index * 32;
	writel(rd + count, descriptor + UMS9117_SBUF_RX_READ * 4);
	/* Complete the space release before checking the producer for a wake. */
	mb();
	*received = count;
	wr = readl(descriptor + UMS9117_SBUF_RX_WRITE * 4);
	if ((u32)(wr - rd - count) > UMS9117_SBUF_RING_BYTES) {
		ret = mailbox_fail("SBUF_STREAM_CHANGED", -EUCLEAN);
		goto out;
	}
	/* The producer can reach full while this copy is in progress. */
	if ((u32)(wr - rd) == UMS9117_SBUF_RING_BYTES)
		sbuf_queue_event(index, UMS9117_SBUF_SPACE_READY);
out:
	local_irq_restore(flags);
	return ret;
}

int ums9117_cm4_mailbox_h4_write(const u8 *data, size_t bytes, size_t *written)
{
	return sbuf_write(0, data, bytes, written, false);
}

int ums9117_cm4_mailbox_h4_read(u8 *data, size_t capacity, size_t *received)
{
	return sbuf_read(0, data, capacity, received);
}

int ums9117_cm4_mailbox_fm_write(const u8 *data, size_t bytes)
{
	size_t written;

	return sbuf_write(1, data, bytes, &written, true);
}

int ums9117_cm4_mailbox_fm_read(u8 *data, size_t capacity, size_t *received)
{
	return sbuf_read(1, data, capacity, received);
}

int ums9117_cm4_mailbox_suspend(void)
{
	u32 ring[UMS9117_SBUF_RING_WORDS];
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
	ret = sbuf_read_live_ring(0, ring);
	if (ret)
		goto out;
	ret = -EBUSY;
	if (ring[UMS9117_SBUF_TX_WRITE] != ring[UMS9117_SBUF_TX_READ] ||
	    ring[UMS9117_SBUF_RX_WRITE] != ring[UMS9117_SBUF_RX_READ] ||
	    mailbox.consumed != mailbox.queued || mailbox.stream.pending ||
	    (mailbox.open.triggered && !mailbox.open.delivered) ||
	    (mailbox.done.triggered && !mailbox.done.delivered) ||
	    (mailbox.stream.notify.data[0] && !mailbox.stream.notify.delivered))
		goto out;
	ret = sbuf_read_live_ring(1, ring);
	if (ret)
		goto out;
	if (ring[UMS9117_SBUF_TX_WRITE] != ring[UMS9117_SBUF_TX_READ] ||
	    ring[UMS9117_SBUF_RX_WRITE] != ring[UMS9117_SBUF_RX_READ]) {
		ret = -EBUSY;
		goto out;
	}
	/* Keep the native channel claimed so its no-suspend IRQ can drain notices. */
	mailbox.suspended = true;
	ret = 0;
out:
	local_irq_restore(flags);
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
	ret = sbuf_read_live_ring(0, ring);
	if (ret)
		goto out;
	mailbox.suspended = false;
out:
	local_irq_restore(flags);
	return ret;
}

int ums9117_cm4_mailbox_stop(void)
{
	unsigned long flags;
	bool pending;

	if (irqs_disabled())
		return -EINVAL;
	local_irq_save(flags);
	if (mailbox.stopped) {
		local_irq_restore(flags);
		return mailbox.stop_error;
	}
	if (mailbox.prepared)
		mailbox_consume_queue();
	mailbox.stopped = true;
	pending = (mailbox.open.triggered && !mailbox.open.delivered) ||
		  (mailbox.done.triggered && !mailbox.done.delivered) ||
		  (mailbox.stream.notify.triggered &&
		   !mailbox.stream.notify.delivered);
	local_irq_restore(flags);
	/* Finish the provider-owned TX while CM4 can still consume its inbox. */
	if (pending)
		mailbox.stop_error = mbox_flush(mailbox.channel,
						UMS9117_MBOX_STOP_TIMEOUT_MS);
	return mailbox.stop_error;
}
