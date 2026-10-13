/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <net/bluetooth/hci_core.h>

#include "marlin2-bluetooth.h"
#include "marlin2-transport.h"

/* Single-threaded external allocation, HCI and synchronous transport fakes. */
struct allocation {
	void *pointer;
	size_t bytes;
};
struct marlin2_channel {
	const struct marlin2_channel_ops *ops;
	void *context;
	bool open;
};
static struct allocation allocations[8];
static struct marlin2_channel channel;
static struct hci_dev *registered_hdev;
static size_t live_allocations;
static size_t setup_baseline;
static unsigned int writes;
static unsigned int waits;
static unsigned int pending_opcode;
static bool fail_allocation;
static int transfer_error;

static bool is_live_storage(const void *data, size_t bytes)
{
	uintptr_t address = (uintptr_t)data;
	size_t index;

	for (index = 0; index < 8; index++) {
		uintptr_t start = (uintptr_t)allocations[index].pointer;
		size_t capacity = allocations[index].bytes;

		if (allocations[index].pointer && address >= start &&
		    address - start <= capacity &&
		    bytes <= capacity - (address - start))
			return true;
	}
	return false;
}

void *kmalloc(size_t bytes, int flags)
{
	void *pointer;
	size_t index;

	(void)flags;
	if (fail_allocation) {
		fail_allocation = false;
		return NULL;
	}
	pointer = malloc(bytes);
	assert(pointer);
	for (index = 0; index < 8; index++) {
		if (!allocations[index].pointer) {
			allocations[index] =
				(struct allocation){ pointer, bytes };
			live_allocations++;
			return pointer;
		}
	}
	abort();
}

void *kzalloc(size_t bytes, int flags)
{
	void *pointer = kmalloc(bytes, flags);

	if (pointer)
		memset(pointer, 0, bytes);
	return pointer;
}

void kfree(void *pointer)
{
	size_t index;

	if (!pointer)
		return;
	for (index = 0; index < 8; index++) {
		if (allocations[index].pointer == pointer) {
			allocations[index] = (struct allocation){ 0 };
			live_allocations--;
			free(pointer);
			return;
		}
	}
	abort();
}

struct hci_dev *hci_alloc_dev(void)
{
	return calloc(1, sizeof(struct hci_dev));
}

void hci_free_dev(struct hci_dev *hdev)
{
	free(hdev);
}

int hci_register_dev(struct hci_dev *hdev)
{
	assert(!registered_hdev);
	registered_hdev = hdev;
	return 0;
}

void hci_unregister_dev(struct hci_dev *hdev)
{
	assert(registered_hdev == hdev);
	registered_hdev = NULL;
}

int hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	(void)hdev;
	(void)skb;
	/* Both setup replies must be consumed by the real receive callback. */
	abort();
}

struct marlin2_channel *
marlin2_channel_register(struct marlin2_transport *transport,
			 enum marlin2_client client,
			 const struct marlin2_channel_ops *ops, void *context)
{
	(void)transport;
	assert(client == MARLIN2_CLIENT_BLUETOOTH);
	assert(!channel.ops);
	channel = (struct marlin2_channel){ .ops = ops, .context = context };
	return &channel;
}

void marlin2_channel_unregister(struct marlin2_channel *registered)
{
	assert(registered == &channel);
	channel = (struct marlin2_channel){ 0 };
}

int marlin2_channel_open(struct marlin2_channel *registered)
{
	assert(registered == &channel);
	channel.open = true;
	return 0;
}

void marlin2_channel_close(struct marlin2_channel *registered)
{
	assert(registered == &channel);
	channel.open = false;
}

int marlin2_channel_send(struct marlin2_channel *registered, u8 subtype,
			 const u8 *payload, size_t bytes)
{
	(void)registered;
	(void)subtype;
	(void)payload;
	(void)bytes;
	/* Setup commands use the direct H4 path. */
	abort();
}

int marlin2_channel_write_bluetooth(struct marlin2_channel *registered,
				    const u8 *data, size_t bytes)
{
	const u8 pskey_command[224] = {
		0x01, 0xa0, 0xfc, 0xdc, [4 ... 219] = 0x5a,
	};
	const u8 enable_command[] = { 0x01, 0xa1, 0xfc, 0x03, 1, 0, 1, 0 };

	assert(registered == &channel && channel.open);
	/* The caller lends DMA-capable storage for the entire synchronous write. */
	assert(is_live_storage(data, bytes));
	if (bytes == sizeof(pskey_command)) {
		assert(!memcmp(data, pskey_command, bytes));
		pending_opcode = 0xfca0;
	} else {
		assert(bytes == sizeof(enable_command));
		assert(!memcmp(data, enable_command, bytes));
		pending_opcode = 0xfca1;
	}
	writes++;
	assert(is_live_storage(data, bytes));
	return transfer_error;
}

unsigned long wait_for_completion_timeout(struct completion *completion,
					  unsigned long timeout)
{
	const u8 pskey_reply[] = {
		4, 0x0e, 10, 1, 0xa0, 0xfc, 0, 0, 0, 0, 0, 0, 0,
	};
	const u8 enable_reply[] = { 4, 0x0e, 6, 1, 0xa1, 0xfc, 1, 0, 1 };

	(void)timeout;
	/* Deliver the controller reply through the production receive callback. */
	waits++;
	if (pending_opcode == 0xfca0)
		channel.ops->receive(channel.context, 0, pskey_reply,
				     sizeof(pskey_reply));
	else {
		assert(pending_opcode == 0xfca1);
		channel.ops->receive(channel.context, 0, enable_reply,
				     sizeof(enable_reply));
	}
	assert(completion->done);
	return 1;
}

static void setup_buffers_survive_transfer_and_failure_allows_retry(int error)
{
	const u8 pskey[220] = { [0 ... 215] = 0x5a };
	struct device dev = { 0 };
	struct marlin2_bluetooth *bt;

	bt = marlin2_bluetooth_register(&dev, NULL, pskey);
	assert(!IS_ERR(bt));
	assert(registered_hdev && registered_hdev->setup);
	assert(!registered_hdev->open(registered_hdev));
	setup_baseline = live_allocations;
	writes = 0;
	waits = 0;
	if (error) {
		fail_allocation = error == -ENOMEM;
		transfer_error = error == -EIO ? error : 0;
		assert(registered_hdev->setup(registered_hdev) == error);
		assert(live_allocations == setup_baseline);
		assert(writes == (error == -EIO ? 1U : 0U));
		assert(!waits);
	}
	transfer_error = 0;
	writes = 0;
	assert(!registered_hdev->setup(registered_hdev));
	assert(writes == 2 && waits == 2);
	assert(live_allocations == setup_baseline);
	marlin2_bluetooth_unregister(bt);
	assert(!live_allocations && !registered_hdev && !channel.ops);
}

int main(void)
{
	setup_buffers_survive_transfer_and_failure_allows_retry(0);
	setup_buffers_survive_transfer_and_failure_allows_retry(-ENOMEM);
	setup_buffers_survive_transfer_and_failure_allows_retry(-EIO);
	puts("Marlin2 Bluetooth setup host buffer cases passed");
	return 0;
}
