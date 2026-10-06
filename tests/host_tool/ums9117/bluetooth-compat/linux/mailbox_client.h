/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_MAILBOX_CLIENT_H
#define BLUETOOTH_HOST_MAILBOX_CLIENT_H

#include <stdbool.h>
#include <linux/device.h>

struct mbox_chan {
	int unused;
};

struct mbox_client {
	struct device *dev;
	bool tx_block;
	unsigned long tx_tout;
	bool knows_txdone;
	void (*rx_callback)(struct mbox_client *client, void *message);
	void (*tx_prepare)(struct mbox_client *client, void *message);
	void (*tx_done)(struct mbox_client *client, void *message, int result);
};

struct mbox_chan *mbox_request_channel_byname(struct mbox_client *client,
					      const char *name);
int mbox_send_message(struct mbox_chan *channel, void *message);
int mbox_flush(struct mbox_chan *channel, unsigned long timeout);
void mbox_free_channel(struct mbox_chan *channel);

#endif
