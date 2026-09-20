// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link the production CM4 mailbox client to a fake Linux mailbox provider and
 * a byte-addressed SIPC window. This checks copied RX values, provider-retained
 * TX messages, and the retained-channel state machine, not MMIO or IRQs.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/device.h>
#include <linux/mailbox_client.h>

#include "cm4-mailbox.h"

#define SIPC_BYTES 0x100000U
#define SIPC_PHYS 0x80000000U
#define RING0_TX_ADDRESS 0x08U
#define RING0_TX_READ 0x10U
#define RING0_TX_WRITE 0x14U
#define RING0_RX_ADDRESS 0x18U
#define RING0_RX_READ 0x20U
#define RING0_RX_WRITE 0x24U

static struct {
	struct mbox_chan channel;
	struct mbox_client *client;
	void *pending_tx;
	void (*cleanup)(void *);
	void *cleanup_data;
	unsigned int sends;
	unsigned int frees;
} provider;

struct mbox_chan *mbox_request_channel_byname(struct mbox_client *client,
					      const char *name)
{
	assert(!provider.client);
	assert(!strcmp(name, "cm4"));
	assert(!client->tx_block);
	assert(client->rx_callback);
	assert(client->tx_done);
	provider.client = client;
	return &provider.channel;
}

int mbox_send_message(struct mbox_chan *channel, void *message)
{
	assert(channel == &provider.channel);
	assert(!provider.pending_tx);
	provider.pending_tx = message;
	return provider.sends++;
}

void mbox_free_channel(struct mbox_chan *channel)
{
	assert(channel == &provider.channel);
	assert(!provider.pending_tx);
	provider.client = NULL;
	provider.frees++;
}

int devm_add_action_or_reset(struct device *device, void (*action)(void *),
			     void *data)
{
	assert(device);
	assert(!provider.cleanup);
	provider.cleanup = action;
	provider.cleanup_data = data;
	return 0;
}

static u32 read_word(const u8 *sipc, size_t offset)
{
	u32 value;

	memcpy(&value, sipc + offset, sizeof(value));
	return value;
}

static void write_word(u8 *sipc, size_t offset, u32 value)
{
	memcpy(sipc + offset, &value, sizeof(value));
}

static int receive_message_and_poll(u32 low, u32 high)
{
	u32 message[2] = { low, high };

	provider.client->rx_callback(provider.client, message);
	message[0] = ~low;
	message[1] = ~high;
	return ums9117_cm4_mailbox_poll();
}

static void *expect_pending_tx(u32 low, u32 high)
{
	const u32 *message = provider.pending_tx;

	assert(message);
	assert(message[0] == low);
	assert(message[1] == high);
	return provider.pending_tx;
}

static void complete_tx(void *expected_message, u32 low, u32 high, int result)
{
	const u32 *words = provider.pending_tx;

	assert(words);
	assert(words == expected_message);
	assert(words[0] == low);
	assert(words[1] == high);
	provider.pending_tx = NULL;
	provider.client->tx_done(provider.client, expected_message, result);
}

static void check_stale_handover_rejected(void)
{
	pid_t child = fork();
	int status;

	assert(child >= 0);
	if (!child) {
		struct device device = {};
		u32 message[2] = { 0xbeee0104, 0 };
		u8 *sipc = calloc(1, SIPC_BYTES);

		assert(sipc);
		assert(!ums9117_cm4_mailbox_init(&device));
		provider.client->rx_callback(provider.client, message);
		write_word(sipc, 0x04, 0xdecafbad);
		assert(ums9117_cm4_mailbox_prepare(sipc) == -EBUSY);
		assert(read_word(sipc, 0x04) == 0xdecafbad);
		provider.cleanup(provider.cleanup_data);
		free(sipc);
		_exit(0);
	}
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

int main(void)
{
	static const u8 outbound[] = { 0x01, 0x02, 0x03 };
	static const u8 inbound[] = { 0x04, 0x05, 0x06 };
	struct device device = {};
	size_t rx_buffer_offset;
	size_t transferred;
	u32 resume_message[2] = { 0x00010404, 0 };
	u8 received[sizeof(inbound)];
	void *pending_message;
	u8 *sipc;

	check_stale_handover_rejected();
	sipc = calloc(1, SIPC_BYTES);
	assert(sipc);
	assert(!ums9117_cm4_mailbox_init(&device));
	assert(provider.client);
	assert(provider.frees == 0);
	assert(!ums9117_cm4_mailbox_prepare(sipc));
	assert(read_word(sipc, 0x04) == 3);
	assert(read_word(sipc, RING0_TX_ADDRESS) == SIPC_PHYS + 0x6c);
	assert(read_word(sipc, RING0_TX_READ) == UINT32_MAX);

	assert(receive_message_and_poll(0xbeee0104, 0) == -EINPROGRESS);
	pending_message = expect_pending_tx(0xbeee0104, 0);
	complete_tx(pending_message, 0xbeee0104, 0, 0);
	assert(receive_message_and_poll(0x00010504, 0) == -EINPROGRESS);
	pending_message = expect_pending_tx(0x00020604, SIPC_PHYS + 0x04);
	complete_tx(pending_message, 0x00020604, SIPC_PHYS + 0x04, 0);
	assert(!ums9117_cm4_mailbox_poll());

	assert(ums9117_cm4_mailbox_h4_begin() == -EINPROGRESS);
	write_word(sipc, RING0_TX_READ, 0);
	assert(!ums9117_cm4_mailbox_h4_begin());
	assert(!ums9117_cm4_mailbox_h4_continue());

	assert(!ums9117_cm4_mailbox_h4_write(outbound, sizeof(outbound),
					     &transferred));
	assert(transferred == sizeof(outbound));
	assert(!memcmp(sipc + 0x6c, outbound, sizeof(outbound)));
	assert(ums9117_cm4_mailbox_suspend() == -EBUSY);
	assert(ums9117_cm4_mailbox_poll() == -EINPROGRESS);
	pending_message = expect_pending_tx(0x00010404, 0);
	assert(ums9117_cm4_mailbox_suspend() == -EBUSY);
	complete_tx(pending_message, 0x00010404, 0, 0);
	assert(!ums9117_cm4_mailbox_poll());
	assert(ums9117_cm4_mailbox_suspend() == -EBUSY);
	write_word(sipc, RING0_TX_READ, read_word(sipc, RING0_TX_WRITE));
	assert(!ums9117_cm4_mailbox_suspend());
	assert(provider.frees == 0);
	assert(ums9117_cm4_mailbox_h4_write(outbound, sizeof(outbound),
					    &transferred) == -EHOSTDOWN);
	provider.client->rx_callback(provider.client, resume_message);
	assert(!ums9117_cm4_mailbox_resume());
	assert(provider.frees == 0);

	rx_buffer_offset = read_word(sipc, RING0_RX_ADDRESS) - SIPC_PHYS;
	memcpy(sipc + rx_buffer_offset, inbound, sizeof(inbound));
	write_word(sipc, RING0_RX_WRITE, sizeof(inbound));
	assert(!ums9117_cm4_mailbox_h4_read(received, sizeof(received),
					    &transferred));
	assert(transferred == sizeof(inbound));
	assert(!memcmp(received, inbound, sizeof(inbound)));
	assert(read_word(sipc, RING0_RX_READ) == sizeof(inbound));

	assert(!ums9117_cm4_mailbox_suspend());
	assert(!ums9117_cm4_mailbox_resume());
	ums9117_cm4_mailbox_stop();
	provider.cleanup(provider.cleanup_data);
	assert(provider.frees == 1);
	free(sipc);
	puts("ums9117 bluetooth mailbox host component: PASS");
	return 0;
}
