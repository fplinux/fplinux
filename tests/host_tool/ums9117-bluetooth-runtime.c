// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link the production HCI runtime. The external HCI core, H4 receiver,
 * workqueue, clock, CM4 owner and mailbox are deterministic single-threaded
 * fakes. FM registration is a fake consumer. This checks transport lifecycle,
 * not H4 reassembly, V4L2 ioctls, kernel concurrency or CM4 MMIO.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <hci_uart.h>
#include <net/bluetooth/hci_core.h>

#include "cm4-hci.h"
#include "cm4-fm.h"
#include "cm4-mailbox.h"
#include "cm4-power.h"

struct ums9117_fm {
	struct device *dev;
};

static struct {
	struct device *dev;
	struct hci_dev *hdev;
	struct ums9117_fm *radio;
	struct delayed_work *work;
	u64 now;
	u8 input[256];
	size_t input_bytes;
	u8 output[1024];
	size_t output_bytes;
	unsigned int register_count;
	unsigned int unregister_count;
	unsigned int radio_register_count;
	unsigned int radio_unregister_count;
	unsigned int owners;
	unsigned int start_count;
	unsigned int stop_count;
	bool powered;
	int owner_error;
	const u8 *restart_prefix;
	size_t restart_prefix_bytes;
	unsigned int suspend_count;
	unsigned int resume_count;
	unsigned int sentinel_events;
	unsigned int received_frames;
	unsigned int read_count;
	bool paused;
	bool peer_busy;
	bool race_connection;
	bool reject_scan;
	int suspend_error;
	u8 fm_output[512];
	size_t fm_output_bytes;
	u8 fm_input[512];
	size_t fm_input_bytes;
	size_t fm_chunk;
	bool fm_sent;
	unsigned int fm_writes;
	unsigned long fm_wait_ms;
} fake;

int ums9117_cm4_get(struct device *dev, enum ums9117_cm4_user user)
{
	int ret;

	assert(dev == fake.dev);
	assert(!(fake.owners & (1U << user)));
	if (fake.owner_error)
		return fake.owner_error;
	if (!fake.powered) {
		ret = ums9117_hci_transport_start(fake.restart_prefix,
						  fake.restart_prefix_bytes);
		if (ret)
			return ret;
		fake.powered = true;
		fake.start_count++;
	}
	fake.owners |= 1U << user;
	return 0;
}

void ums9117_cm4_put(struct device *dev, enum ums9117_cm4_user user)
{
	assert(dev == fake.dev);
	assert(fake.owners & (1U << user));
	fake.owners &= ~(1U << user);
	if (!fake.owners) {
		ums9117_hci_transport_stop();
		fake.powered = false;
		fake.stop_count++;
		fake.input_bytes = 0;
		fake.fm_input_bytes = 0;
		fake.fm_sent = false;
	}
}

struct ums9117_fm *ums9117_fm_register(struct device *dev)
{
	assert(dev == fake.dev && !fake.radio);
	fake.radio = calloc(1, sizeof(*fake.radio));
	assert(fake.radio);
	fake.radio->dev = dev;
	fake.radio_register_count++;
	return fake.radio;
}

void ums9117_fm_unregister(struct ums9117_fm *radio)
{
	if (!radio)
		return;
	assert(radio == fake.radio);
	fake.radio_unregister_count++;
	fake.radio = NULL;
	free(radio);
}

static void pump(void)
{
	if (!fake.work || !fake.work->pending)
		return;
	fake.work->pending = false;
	fake.work->work.function(&fake.work->work);
}

unsigned long wait_for_completion_timeout(struct completion *completion,
					  unsigned long timeout)
{
	unsigned long elapsed;

	fake.fm_wait_ms = timeout;
	for (elapsed = 0; elapsed < timeout && !completion->done; elapsed++)
		pump();
	return completion->done;
}

bool schedule_delayed_work(struct delayed_work *work, unsigned long delay)
{
	bool was_pending = work->pending;

	(void)delay;
	fake.work = work;
	work->pending = true;
	return !was_pending;
}

bool cancel_delayed_work_sync(struct delayed_work *work)
{
	bool was_pending = work->pending;

	work->pending = false;
	return was_pending;
}

u64 ktime_get_ns(void)
{
	return fake.now;
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
	assert(minimum <= maximum);
	fake.now += NSEC_PER_SEC;
	pump();
}

struct hci_dev *hci_alloc_dev(void)
{
	struct hci_dev *hdev = calloc(1, sizeof(*hdev));

	assert(hdev);
	hdev->cmd_cnt = 1;
	return hdev;
}

void hci_free_dev(struct hci_dev *hdev)
{
	free(hdev);
}

int hci_register_dev(struct hci_dev *hdev)
{
	assert(!fake.hdev);
	fake.hdev = hdev;
	fake.register_count++;
	assert(hdev->open(hdev) == 0);
	hdev->flags |= 1UL << HCI_UP;
	return 0;
}

void hci_unregister_dev(struct hci_dev *hdev)
{
	assert(hdev == fake.hdev);
	fake.unregister_count++;
	assert(hdev->close(hdev) == 0);
	fake.hdev = NULL;
}

int hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	int ret = 0;

	assert(hdev == fake.hdev);
	fake.received_frames++;
	if (skb->type == 4 && skb->len == 3 &&
	    !memcmp(skb->data, "\xff\x01\x42", 3))
		fake.sentinel_events++;
	if (skb->type == 4 && skb->len == 2 &&
	    !memcmp(skb->data, "\xfe\x00", 2))
		ret = -EIO;
	kfree_skb(skb);
	return ret;
}

/*
 * This replaces the external native H4 receiver for adapter tests. It accepts
 * one whole fixture packet and deliberately does not inspect H4 header lengths
 * or implement H4 reassembly; native parser behavior belongs to the kernel
 * boundary. One scripted retained event exposes the transport's close/reopen
 * discard policy without becoming a parser test.
 */
struct sk_buff *h4_recv_buf(struct hci_uart *hu, struct sk_buff *skb,
			    const unsigned char *buffer, int count,
			    const struct h4_recv_pkt *pkts, int pkts_count)
{
	struct sk_buff *frame;
	int index;

	if (!count)
		return skb;
	if (!skb && count == 1 && buffer[0] == HCI_EVENT_PKT) {
		frame = bt_skb_alloc(0, GFP_KERNEL);
		hci_skb_pkt_type(frame) = HCI_EVENT_PKT;
		return frame;
	}
	if (skb) {
		assert(hci_skb_pkt_type(skb) == HCI_EVENT_PKT && count == 3);
		frame = bt_skb_alloc(count, GFP_KERNEL);
		hci_skb_pkt_type(frame) = hci_skb_pkt_type(skb);
		kfree_skb(skb);
		skb_put_data(frame, buffer, count);
		for (index = 0; index < pkts_count; index++)
			if (pkts[index].type == hci_skb_pkt_type(frame))
				break;
		assert(index < pkts_count);
		(void)pkts[index].recv(hu->hdev, frame);
		return NULL;
	}
	if (buffer[0] == 0)
		return ERR_PTR(-EILSEQ);
	for (index = 0; index < pkts_count; index++)
		if (pkts[index].type == buffer[0])
			break;
	assert(index < pkts_count);
	frame = bt_skb_alloc((size_t)count - 1, GFP_KERNEL);
	hci_skb_pkt_type(frame) = buffer[0];
	skb_put_data(frame, buffer + 1, (size_t)count - 1);
	/* h4_recv_buf ignores receive callback returns in the native implementation. */
	(void)pkts[index].recv(hu->hdev, frame);
	return NULL;
}

static int send_packet(u8 type, const u8 *data, size_t bytes)
{
	struct sk_buff *skb = bt_skb_alloc(bytes, 0);
	int ret;

	skb->type = type;
	skb_put_data(skb, data, bytes);
	ret = fake.hdev->send(fake.hdev, skb);
	if (ret)
		kfree_skb(skb);
	return ret;
}

static void controller_input(const u8 *data, size_t bytes)
{
	assert(bytes <= sizeof(fake.input) - fake.input_bytes);
	memcpy(fake.input + fake.input_bytes, data, bytes);
	fake.input_bytes += bytes;
}

static void native_scan_command(bool enable, bool reject)
{
	const u8 command[] = { 0x1a, 0x0c, 0x01, enable ? 0x03 : 0x00 };
	const u8 reply[] = {
		0x04, 0x0e, 0x04, 0x01, 0x1a, 0x0c, reject ? 0x0c : 0x00
	};

	assert(send_packet(1, command, sizeof(command)) == 0);
	pump();
	controller_input(reply, sizeof(reply));
	pump();
}

int hci_suspend_dev(struct hci_dev *hdev)
{
	const u8 create_connection[] = { 0x05, 0x04, 0x00 };

	assert(hdev == fake.hdev && !hdev->lock.held);
	fake.suspend_count++;
	/* Models native suspend's destructive boundary: existing links would drop. */
	hdev->connections = 0;
	if (fake.race_connection)
		assert(send_packet(1, create_connection,
				   sizeof(create_connection)) == -EBUSY);
	native_scan_command(false, fake.reject_scan);
	/* Native suspend ignores some scan-command status errors. */
	return fake.suspend_error;
}

int hci_resume_dev(struct hci_dev *hdev)
{
	assert(hdev == fake.hdev && !hdev->lock.held);
	fake.resume_count++;
	native_scan_command(true, false);
	return 0;
}

int ums9117_cm4_mailbox_poll(void)
{
	assert(fake.powered && !fake.paused);
	return 0;
}

int ums9117_cm4_mailbox_h4_write(const u8 *data, size_t bytes, size_t *written)
{
	assert(fake.powered && !fake.paused);
	assert(bytes <= sizeof(fake.output) - fake.output_bytes);
	memcpy(fake.output + fake.output_bytes, data, bytes);
	fake.output_bytes += bytes;
	*written = bytes;
	return 0;
}

int ums9117_cm4_mailbox_h4_read(u8 *data, size_t capacity, size_t *received)
{
	size_t bytes = fake.input_bytes < capacity ? fake.input_bytes :
						     capacity;

	assert(fake.powered && !fake.paused);
	fake.read_count++;
	memcpy(data, fake.input, bytes);
	memmove(fake.input, fake.input + bytes, fake.input_bytes - bytes);
	fake.input_bytes -= bytes;
	*received = bytes;
	return 0;
}

int ums9117_cm4_mailbox_fm_write(const u8 *data, size_t bytes)
{
	assert(fake.powered && !fake.paused);
	assert(bytes <= sizeof(fake.fm_output));
	memcpy(fake.fm_output, data, bytes);
	fake.fm_output_bytes = bytes;
	fake.fm_sent = true;
	fake.fm_writes++;
	return 0;
}

int ums9117_cm4_mailbox_fm_read(u8 *data, size_t capacity, size_t *received)
{
	size_t bytes = fake.fm_sent ? fake.fm_input_bytes : 0;

	assert(fake.powered && !fake.paused);
	if (bytes > capacity)
		bytes = capacity;
	if (fake.fm_chunk && bytes > fake.fm_chunk)
		bytes = fake.fm_chunk;
	memcpy(data, fake.fm_input, bytes);
	memmove(fake.fm_input, fake.fm_input + bytes,
		fake.fm_input_bytes - bytes);
	fake.fm_input_bytes -= bytes;
	*received = bytes;
	return 0;
}

int ums9117_cm4_mailbox_suspend(void)
{
	assert(fake.powered);
	if (fake.peer_busy || fake.input_bytes)
		return -EBUSY;
	fake.paused = true;
	return 0;
}

int ums9117_cm4_mailbox_resume(void)
{
	assert(fake.powered);
	fake.paused = false;
	return 0;
}

static void start(void)
{
	static struct device device;

	memset(&fake, 0, sizeof(fake));
	fake.dev = &device;
	fake.powered = true;
	fake.start_count = 1;
	assert(ums9117_hci_runtime_register(&device, NULL, 0) == 0);
	pump();
}

static void stop(void)
{
	assert(fake.register_count == 1 && !fake.unregister_count);
	ums9117_hci_runtime_unregister();
	assert(fake.unregister_count == 1);
	assert(fake.radio_register_count == 1 &&
	       fake.radio_unregister_count == 1);
	assert(!(fake.owners & (1U << UMS9117_CM4_BLUETOOTH)));
}

static void data_still_flows(void)
{
	const u8 acl[] = { 0x01, 0x00, 0x01, 0x00, 0x42 };
	const u8 expected[] = { 0x02, 0x01, 0x00, 0x01, 0x00, 0x42, 0, 0 };
	size_t before = fake.output_bytes;

	assert(send_packet(2, acl, sizeof(acl)) == 0);
	pump();
	assert(fake.output_bytes == before + sizeof(expected));
	assert(!memcmp(fake.output + before, expected, sizeof(expected)));
}

static void malformed_transmit_never_reaches_peer(void)
{
	const u8 incomplete_command[] = { 0x03, 0x0c, 0x01 };
	size_t before = fake.output_bytes;

	assert(send_packet(HCI_COMMAND_PKT, incomplete_command,
			   sizeof(incomplete_command)) == -EPROTO);
	assert(fake.output_bytes == before);
}

static void active_connection_is_preserved(void)
{
	start();
	fake.hdev->connections = 1;
	assert(ums9117_hci_suspend_prepare() == -EBUSY);
	assert(fake.hdev->connections == 1 && !fake.suspend_count);
	data_still_flows();
	stop();
}

static void controller_initialization_is_not_interrupted(void)
{
	start();
	fake.hdev->flags |= 1UL << HCI_INIT;
	assert(ums9117_hci_suspend_prepare() == -EBUSY);
	assert(!fake.suspend_count);
	fake.hdev->flags &= ~(1UL << HCI_INIT);
	data_still_flows();
	stop();
}

static void failed_prepare_reopens_admission(void)
{
	const int expected[] = { -EBUSY, -EREMOTEIO, -EIO };
	size_t case_index;

	for (case_index = 0; case_index < 3; case_index++) {
		start();
		fake.race_connection = case_index == 0;
		fake.reject_scan = case_index == 1;
		fake.suspend_error = case_index == 2 ? -EIO : 0;
		assert(ums9117_hci_suspend_prepare() == expected[case_index]);
		assert(fake.suspend_count == 1 && fake.resume_count == 1);
		assert(!fake.paused);
		data_still_flows();
		stop();
	}
}

static void retained_resume_preserves_controller_and_input(void)
{
	const u8 sentinel[] = { 0x04, 0xff, 0x01, 0x42 };
	const u8 query[] = { 0x01, 0x10, 0x00 };
	unsigned int reads;

	start();
	assert(ums9117_hci_suspend_prepare() == 0);
	assert(ums9117_hci_suspend() == 0);
	reads = fake.read_count;
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.read_count == reads && !fake.sentinel_events);
	assert(send_packet(1, query, sizeof(query)) == -EHOSTDOWN);
	assert(ums9117_hci_resume() == 0);
	assert(ums9117_hci_post_suspend() == 0);
	assert(fake.sentinel_events == 1);
	assert(fake.suspend_count == 1 && fake.resume_count == 1);
	data_still_flows();
	stop();
}

static void closed_controller_frames_are_discarded_until_reopened(void)
{
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	assert(ums9117_hci_fm_hold() == 0);
	assert(fake.hdev->close(fake.hdev) == 0);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(!fake.sentinel_events);
	assert(fake.hdev->open(fake.hdev) == 0);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.sentinel_events == 1);
	ums9117_hci_fm_release();
	stop();
}

static void close_discards_a_frame_reassembled_after_reopening(void)
{
	const u8 event_prefix[] = { HCI_EVENT_PKT };
	const u8 event_remainder[] = { 0xff, 0x01, 0x42 };
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	assert(ums9117_hci_fm_hold() == 0);
	assert(fake.hdev->close(fake.hdev) == 0);
	controller_input(event_prefix, sizeof(event_prefix));
	pump();
	assert(fake.hdev->open(fake.hdev) == 0);
	controller_input(event_remainder, sizeof(event_remainder));
	pump();
	assert(!fake.sentinel_events);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.sentinel_events == 1);
	ums9117_hci_fm_release();
	stop();
}

static void receive_callback_failure_stops_following_delivery(void)
{
	const u8 rejected[] = { HCI_EVENT_PKT, 0xfe, 0x00 };
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	controller_input(rejected, sizeof(rejected));
	pump();
	assert(fake.received_frames == 1);
	assert(fake.hdev->stat.err_rx == 1);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.received_frames == 1 && !fake.sentinel_events);
	stop();
}

static void native_receiver_error_stops_following_delivery(void)
{
	const u8 invalid_type[] = { 0 };
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	controller_input(invalid_type, sizeof(invalid_type));
	pump();
	assert(!fake.received_frames && fake.hdev->stat.err_rx == 1);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(!fake.received_frames && !fake.sentinel_events);
	stop();
}

static void undrained_peer_veto_is_retryable(void)
{
	start();
	assert(ums9117_hci_suspend_prepare() == 0);
	fake.peer_busy = true;
	assert(ums9117_hci_suspend() == -EBUSY);
	assert(!fake.paused);
	assert(ums9117_hci_post_suspend() == 0);
	data_still_flows();
	fake.peer_busy = false;
	assert(ums9117_hci_suspend_prepare() == 0);
	assert(ums9117_hci_suspend() == 0);
	assert(ums9117_hci_post_suspend() == 0);
	data_still_flows();
	stop();
}

static void fm_split_completion_and_seek_event_preserve_bluetooth(void)
{
	const u8 seek_request[] = { 0x01, 0x8c, 0xfc, 0x04,
				    0x04, 0x2e, 0x22, 0x01 };
	const u8 seek_replies[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc,
				    0x00, 0x04, 0xff, 0x06, 0x30, 0x00,
				    0x48, 0x1c, 0x10, 0x27 };
	const u8 seek_event[] = { 0x04, 0xff, 0x06, 0x30, 0x00,
				  0x48, 0x1c, 0x10, 0x27 };
	const u8 payload[] = { 0x2e, 0x22, 0x01 };
	u8 reply[9];
	size_t chunk;

	/* Exercise both byte-split events and two events sharing one ring read. */
	for (chunk = 1; chunk <= sizeof(seek_replies); chunk++) {
		start();
		memcpy(fake.fm_input, seek_replies, sizeof(seek_replies));
		fake.fm_input_bytes = sizeof(seek_replies);
		fake.fm_chunk = chunk;
		assert(ums9117_hci_fm_command(0x04, payload, sizeof(payload),
					      reply, sizeof(reply), true) == 9);
		assert(fake.fm_writes == 1);
		assert(fake.fm_wait_ms == 15000);
		assert(fake.fm_output_bytes == sizeof(seek_request));
		assert(!memcmp(fake.fm_output, seek_request,
			       sizeof(seek_request)));
		assert(!memcmp(reply, seek_event, sizeof(seek_event)));
		data_still_flows();
		stop();
	}
}

static void fm_timeout_quarantines_late_replies_without_stopping_bluetooth(void)
{
	const u8 late_reply[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x01 };
	const u8 frequency[] = { 0x2e, 0x22 };
	u8 reply[11];

	start();
	assert(!ums9117_hci_fm_hold());
	assert(ums9117_hci_fm_command(0x01, frequency, sizeof(frequency), reply,
				      sizeof(reply), false) == -ETIMEDOUT);
	assert(fake.fm_wait_ms == 1000);
	memcpy(fake.fm_input, late_reply, sizeof(late_reply));
	fake.fm_input_bytes = sizeof(late_reply);
	pump();
	assert(!fake.fm_input_bytes);
	assert(ums9117_hci_fm_command(0x01, frequency, sizeof(frequency), reply,
				      sizeof(reply), false) == -ETIMEDOUT);
	assert(fake.fm_writes == 1);
	data_still_flows();
	assert(ums9117_hci_suspend_prepare() == -EBUSY);
	stop();
}

static void fm_commands_preserve_config_bytes_and_short_error_responses(void)
{
	const u8 enable_reply[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	const u8 tune_error[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x01 };
	const u8 expected_header[] = {
		0x01, 0x8c, 0xfc, 0x83, 0x00, 0x2e, 0x22
	};
	u8 payload[130] = { 0x2e, 0x22 };
	u8 reply[11];
	size_t index;

	/* Synthetic calibration bytes, including embedded H4-looking values. */
	for (index = 2; index < sizeof(payload); index++)
		payload[index] = index - 2;
	start();
	memcpy(fake.fm_input, enable_reply, sizeof(enable_reply));
	fake.fm_input_bytes = sizeof(enable_reply);
	assert(ums9117_hci_fm_command(0, payload, sizeof(payload), reply, 7,
				      false) == 7);
	assert(fake.fm_output_bytes == 135);
	assert(!memcmp(fake.fm_output, expected_header,
		       sizeof(expected_header)));
	assert(!memcmp(fake.fm_output + 7, payload + 2, 128));
	assert(!memcmp(reply, enable_reply, sizeof(enable_reply)));
	fake.fm_sent = false;
	memcpy(fake.fm_input, tune_error, sizeof(tune_error));
	fake.fm_input_bytes = sizeof(tune_error);
	assert(ums9117_hci_fm_command(1, payload, 2, reply, sizeof(reply),
				      false) == 7);
	assert(!memcmp(reply, tune_error, sizeof(tune_error)));
	assert(fake.fm_writes == 2);
	data_still_flows();
	stop();
}

static void enabled_fm_vetoes_suspend_until_disable(void)
{
	start();
	assert(!ums9117_hci_fm_hold());
	assert(ums9117_hci_suspend_prepare() == -EBUSY);
	data_still_flows();
	ums9117_hci_fm_release();
	assert(!ums9117_hci_suspend_prepare());
	assert(!ums9117_hci_suspend());
	assert(!ums9117_hci_post_suspend());
	stop();
}

static void power_cycles_keep_devices_and_allow_stopped_suspend(void)
{
	const u8 command[] = { 0x01, 0x10, 0x00 };
	struct ums9117_fm *radio;
	struct hci_dev *hdev;
	unsigned int reads;
	unsigned int cycle;

	start();
	hdev = fake.hdev;
	radio = fake.radio;
	for (cycle = 0; cycle < 3; cycle++) {
		assert(hdev->close(hdev) == 0);
		assert(!fake.owners && !fake.powered && !fake.work->pending);
		reads = fake.read_count;
		pump();
		assert(fake.read_count == reads);
		assert(send_packet(HCI_COMMAND_PKT, command, sizeof(command)) ==
		       -ESHUTDOWN);
		assert(ums9117_hci_suspend_prepare() == 0);
		assert(ums9117_hci_suspend() == 0);
		assert(ums9117_hci_resume() == 0);
		assert(ums9117_hci_post_suspend() == 0);
		assert(!fake.suspend_count && !fake.resume_count);
		assert(!fake.powered && fake.read_count == reads);
		assert(hdev->open(hdev) == 0);
		assert(fake.owners == (1U << UMS9117_CM4_BLUETOOTH));
		assert(fake.hdev == hdev && fake.radio == radio);
		assert(fake.register_count == 1 &&
		       fake.radio_register_count == 1);
		assert(!fake.unregister_count && !fake.radio_unregister_count);
		data_still_flows();
	}
	assert(fake.start_count == 4 && fake.stop_count == 3);
	stop();
	assert(!fake.owners && !fake.powered && !fake.work->pending);
}

static void fm_and_bluetooth_hold_power_independently(void)
{
	const u8 command_reply[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	u8 reply[7];

	start();
	assert(ums9117_hci_fm_hold() == 0);
	assert(fake.hdev->close(fake.hdev) == 0);
	assert(fake.owners == (1U << UMS9117_CM4_FM));
	assert(fake.powered && !fake.stop_count);
	memcpy(fake.fm_input, command_reply, sizeof(command_reply));
	fake.fm_input_bytes = sizeof(command_reply);
	assert(ums9117_hci_fm_command(0, NULL, 0, reply, sizeof(reply),
				      false) == 7);
	assert(!memcmp(reply, command_reply, sizeof(reply)));
	assert(ums9117_hci_suspend_prepare() == -EBUSY);
	assert(fake.hdev->open(fake.hdev) == 0);
	ums9117_hci_fm_release();
	assert(fake.owners == (1U << UMS9117_CM4_BLUETOOTH));
	assert(fake.powered && !fake.stop_count);
	data_still_flows();
	assert(fake.hdev->close(fake.hdev) == 0);
	assert(!fake.powered && fake.stop_count == 1);
	assert(ums9117_hci_fm_hold() == 0);
	assert(fake.owners == (1U << UMS9117_CM4_FM));
	assert(fake.powered && fake.start_count == 2);
	memcpy(fake.fm_input, command_reply, sizeof(command_reply));
	fake.fm_input_bytes = sizeof(command_reply);
	assert(ums9117_hci_fm_command(0, NULL, 0, reply, sizeof(reply),
				      false) == 7);
	ums9117_hci_fm_release();
	assert(!fake.owners && !fake.powered && fake.stop_count == 2);
	stop();
	assert(fake.stop_count == 2);
}

static void failed_start_leaves_both_users_unclaimed(void)
{
	start();
	assert(fake.hdev->close(fake.hdev) == 0);
	fake.owner_error = -EIO;
	assert(fake.hdev->open(fake.hdev) == -EIO);
	assert(ums9117_hci_fm_hold() == -EIO);
	assert(!fake.owners && !fake.powered);
	assert(fake.start_count == 1 && fake.stop_count == 1);
	fake.owner_error = 0;
	assert(fake.hdev->open(fake.hdev) == 0);
	data_still_flows();
	stop();
}

static void failed_admission_releases_only_the_new_owner(void)
{
	const u8 invalid_type[] = { 0 };

	start();
	assert(ums9117_hci_fm_hold() == 0);
	assert(fake.hdev->close(fake.hdev) == 0);
	controller_input(invalid_type, sizeof(invalid_type));
	pump();
	assert(fake.hdev->open(fake.hdev) == -EILSEQ);
	assert(fake.owners == (1U << UMS9117_CM4_FM));
	assert(fake.powered && !fake.stop_count);
	stop();

	start();
	controller_input(invalid_type, sizeof(invalid_type));
	pump();
	assert(ums9117_hci_fm_hold() == -EILSEQ);
	assert(fake.owners == (1U << UMS9117_CM4_BLUETOOTH));
	assert(fake.powered && !fake.stop_count);
	stop();
}

static void physical_restart_discards_the_old_partial_frame(void)
{
	const u8 event_prefix[] = { HCI_EVENT_PKT };
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	controller_input(event_prefix, sizeof(event_prefix));
	pump();
	assert(fake.hdev->close(fake.hdev) == 0);
	assert(fake.hdev->open(fake.hdev) == 0);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.sentinel_events == 1);
	stop();
}

static void restarted_transport_discards_a_partial_prologue_event(void)
{
	const u8 event_prefix[] = { HCI_EVENT_PKT };
	const u8 event_remainder[] = { 0xff, 0x01, 0x42 };
	const u8 sentinel[] = { HCI_EVENT_PKT, 0xff, 0x01, 0x42 };

	start();
	assert(fake.hdev->close(fake.hdev) == 0);
	fake.restart_prefix = event_prefix;
	fake.restart_prefix_bytes = sizeof(event_prefix);
	assert(fake.hdev->open(fake.hdev) == 0);
	controller_input(event_remainder, sizeof(event_remainder));
	pump();
	assert(!fake.sentinel_events);
	controller_input(sentinel, sizeof(sentinel));
	pump();
	assert(fake.sentinel_events == 1);
	stop();
}

int main(void)
{
	active_connection_is_preserved();
	controller_initialization_is_not_interrupted();
	failed_prepare_reopens_admission();
	retained_resume_preserves_controller_and_input();
	undrained_peer_veto_is_retryable();
	start();
	malformed_transmit_never_reaches_peer();
	stop();
	closed_controller_frames_are_discarded_until_reopened();
	close_discards_a_frame_reassembled_after_reopening();
	receive_callback_failure_stops_following_delivery();
	native_receiver_error_stops_following_delivery();
	fm_split_completion_and_seek_event_preserve_bluetooth();
	fm_timeout_quarantines_late_replies_without_stopping_bluetooth();
	fm_commands_preserve_config_bytes_and_short_error_responses();
	enabled_fm_vetoes_suspend_until_disable();
	power_cycles_keep_devices_and_allow_stopped_suspend();
	fm_and_bluetooth_hold_power_independently();
	failed_start_leaves_both_users_unclaimed();
	failed_admission_releases_only_the_new_owner();
	physical_restart_discards_the_old_partial_frame();
	restarted_transport_discards_a_partial_prologue_event();
	puts("HCI transport lifecycle component checks passed");
	return 0;
}
