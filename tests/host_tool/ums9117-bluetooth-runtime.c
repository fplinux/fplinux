// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link the production HCI runtime and H4 parser. The external HCI core,
 * workqueue, clock and mailbox are deterministic single-threaded fakes.
 * This checks retained transport behavior, not kernel concurrency or CM4 MMIO.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <net/bluetooth/hci_core.h>

#include "cm4-hci.h"
#include "cm4-mailbox.h"

static struct {
	struct hci_dev *hdev;
	struct delayed_work *work;
	u64 now;
	u8 input[256];
	size_t input_bytes;
	u8 output[1024];
	size_t output_bytes;
	unsigned int register_count;
	unsigned int unregister_count;
	unsigned int suspend_count;
	unsigned int resume_count;
	unsigned int sentinel_events;
	unsigned int read_count;
	bool paused;
	bool peer_busy;
	bool race_connection;
	bool reject_scan;
	int suspend_error;
} fake;

static void pump(void)
{
	if (!fake.work || !fake.work->pending)
		return;
	fake.work->pending = false;
	fake.work->work.function(&fake.work->work);
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
	assert(hdev == fake.hdev);
	if (skb->type == 4 && skb->len == 3 &&
	    !memcmp(skb->data, "\xff\x01\x42", 3))
		fake.sentinel_events++;
	kfree_skb(skb);
	return 0;
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
	assert(!fake.paused);
	return 0;
}

int ums9117_cm4_mailbox_h4_write(const u8 *data, size_t bytes, size_t *written)
{
	assert(!fake.paused);
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

	assert(!fake.paused);
	fake.read_count++;
	memcpy(data, fake.input, bytes);
	memmove(fake.input, fake.input + bytes, fake.input_bytes - bytes);
	fake.input_bytes -= bytes;
	*received = bytes;
	return 0;
}

int ums9117_cm4_mailbox_suspend(void)
{
	if (fake.peer_busy || fake.input_bytes)
		return -EBUSY;
	fake.paused = true;
	return 0;
}

int ums9117_cm4_mailbox_resume(void)
{
	fake.paused = false;
	return 0;
}

static void start(void)
{
	static struct device device;

	memset(&fake, 0, sizeof(fake));
	assert(ums9117_hci_runtime_register(&device, NULL, 0) == 0);
	pump();
}

static void stop(void)
{
	assert(fake.register_count == 1 && !fake.unregister_count);
	ums9117_hci_runtime_unregister();
	assert(fake.unregister_count == 1);
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

int main(void)
{
	active_connection_is_preserved();
	controller_initialization_is_not_interrupted();
	failed_prepare_reopens_admission();
	retained_resume_preserves_controller_and_input();
	undrained_peer_veto_is_retryable();
	puts("HCI retained transport component checks passed");
	return 0;
}
