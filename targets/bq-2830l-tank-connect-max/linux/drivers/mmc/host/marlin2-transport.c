// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bug.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include "marlin2-packet.h"
#include "marlin2-transport.h"
#include "marlin2-transport-wire.h"

#define MARLIN2_TRANSPORT_PACKET_PORT 0x20
#define MARLIN2_TRANSPORT_TX_MAX_BYTES 4096U

struct marlin2_channel {
	struct marlin2_transport *transport;
	/* Closing one consumer waits only for its own current callback. */
	struct mutex callback_lock;
	const struct marlin2_channel_ops *ops;
	void *context;
	enum marlin2_client client;
	bool active;
};

struct marlin2_transport {
	struct device *dev;
	const struct marlin2_transport_radio_ops *radio_ops;
	void *radio_context;
	/* Serializes client opens, closes, registration and the power lifetime. */
	struct mutex lifetime_lock;
	/* Both packet FIFO and Bluetooth direct writes share this host claim. */
	struct mutex io_lock;
	struct marlin2_channel channels[MARLIN2_CLIENT_COUNT];
	struct sdio_func *func;
	u8 *rx_packet;
	u32 users;
	int irq;
	int io_error;
	bool running;
	bool irq_requested;
};

static bool channel_active(struct marlin2_channel *channel)
{
	/* Pair with open_channel() to observe the published consumer state. */
	return smp_load_acquire(&channel->active);
}

static int receive_message(void *context,
			   const struct marlin2_transport_message *message)
{
	struct marlin2_transport *transport = context;
	struct marlin2_channel *channel;

	if (message->type >= MARLIN2_CLIENT_COUNT)
		return 0;
	channel = &transport->channels[message->type];
	mutex_lock(&channel->callback_lock);
	if (channel_active(channel))
		channel->ops->receive(channel->context, message->subtype,
				      message->data, message->length);
	mutex_unlock(&channel->callback_lock);
	return 0;
}

static void receive_error(struct marlin2_transport *transport, int error)
{
	unsigned int index;

	WRITE_ONCE(transport->io_error, error);
	for (index = 0; index < MARLIN2_CLIENT_COUNT; index++) {
		struct marlin2_channel *channel = &transport->channels[index];

		mutex_lock(&channel->callback_lock);
		if (channel_active(channel) && channel->ops->error)
			channel->ops->error(channel->context, error);
		mutex_unlock(&channel->callback_lock);
	}
}

static irqreturn_t receive_irq(int irq, void *context)
{
	struct marlin2_transport *transport = context;
	u32 next_bytes = 0;
	size_t bytes = MARLIN2_PACKET_RX_INITIAL_BYTES;
	int ret;

	if (!READ_ONCE(transport->running))
		return IRQ_HANDLED;
	do {
		mutex_lock(&transport->io_lock);
		sdio_claim_host(transport->func);
		ret = sdio_readsb(transport->func, transport->rx_packet,
				  MARLIN2_TRANSPORT_PACKET_PORT, bytes);
		sdio_release_host(transport->func);
		mutex_unlock(&transport->io_lock);
		if (ret)
			break;
		ret = marlin2_transport_aggregate_decode(transport->rx_packet,
							 bytes, receive_message,
							 transport,
							 &next_bytes);
		if (ret)
			break;
		bytes = marlin2_packet_next_read(next_bytes);
	} while (next_bytes && READ_ONCE(transport->running));
	if (ret) {
		disable_irq_nosync(irq);
		receive_error(transport, ret);
		dev_err(transport->dev, "radio receive failed: %pe\n",
			ERR_PTR(ret));
	}
	return IRQ_HANDLED;
}

static void stop_transport(struct marlin2_transport *transport)
{
	WRITE_ONCE(transport->running, false);
	if (transport->irq_requested) {
		free_irq(transport->irq, transport);
		transport->irq_requested = false;
	}
	mutex_lock(&transport->io_lock);
	transport->radio_ops->release(transport->radio_context);
	transport->func = NULL;
	mutex_unlock(&transport->io_lock);
}

static int start_transport(struct marlin2_transport *transport)
{
	struct gpio_desc *rx_request;
	struct mmc_host *host;
	int ret;

	ret = transport->radio_ops->acquire(transport->radio_context,
					    &transport->func, &rx_request);
	if (ret)
		return ret;
	host = transport->func->card->host;
	if (transport->func->num != 1 || !rx_request ||
	    transport->func->cur_blksize != 512 ||
	    !transport->func->card->cccr.multi_block ||
	    host->max_req_size < MARLIN2_PACKET_RX_MAX_BYTES ||
	    host->max_seg_size < MARLIN2_PACKET_RX_MAX_BYTES ||
	    host->max_blk_count < MARLIN2_PACKET_RX_MAX_BYTES / 512) {
		ret = -EPROTONOSUPPORT;
		goto out_release;
	}
	transport->irq = gpiod_to_irq(rx_request);
	if (transport->irq < 0) {
		ret = transport->irq;
		goto out_release;
	}
	WRITE_ONCE(transport->io_error, 0);
	WRITE_ONCE(transport->running, true);
	ret = request_threaded_irq(transport->irq, NULL, receive_irq,
				   IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
				   "marlin2-radio", transport);
	if (ret)
		goto out_release;
	transport->irq_requested = true;
	return 0;

out_release:
	WRITE_ONCE(transport->running, false);
	transport->radio_ops->release(transport->radio_context);
	transport->func = NULL;
	return ret;
}

struct marlin2_transport *
marlin2_transport_create(struct device *dev,
			 const struct marlin2_transport_radio_ops *radio_ops,
			 void *radio_context)
{
	struct marlin2_transport *transport;
	unsigned int index;

	if (!radio_ops || !radio_ops->acquire || !radio_ops->release)
		return ERR_PTR(-EINVAL);
	transport = kzalloc_obj(*transport, GFP_KERNEL);
	if (!transport)
		return ERR_PTR(-ENOMEM);
	transport->rx_packet = kmalloc(MARLIN2_PACKET_RX_MAX_BYTES, GFP_KERNEL);
	if (!transport->rx_packet) {
		kfree(transport);
		return ERR_PTR(-ENOMEM);
	}
	transport->dev = dev;
	transport->radio_ops = radio_ops;
	transport->radio_context = radio_context;
	mutex_init(&transport->lifetime_lock);
	mutex_init(&transport->io_lock);
	for (index = 0; index < MARLIN2_CLIENT_COUNT; index++) {
		mutex_init(&transport->channels[index].callback_lock);
		transport->channels[index].transport = transport;
		transport->channels[index].client = index;
	}
	return transport;
}

void marlin2_transport_destroy(struct marlin2_transport *transport)
{
	unsigned int index;

	for (index = 0; index < MARLIN2_CLIENT_COUNT; index++)
		if (WARN_ON(transport->channels[index].ops))
			return;
	kfree(transport->rx_packet);
	kfree(transport);
}

struct marlin2_channel *
marlin2_channel_register(struct marlin2_transport *transport,
			 enum marlin2_client client,
			 const struct marlin2_channel_ops *ops, void *context)
{
	struct marlin2_channel *channel;

	if ((unsigned int)client >= MARLIN2_CLIENT_COUNT || !ops ||
	    !ops->receive)
		return ERR_PTR(-EINVAL);
	mutex_lock(&transport->lifetime_lock);
	channel = &transport->channels[client];
	if (channel->ops) {
		channel = ERR_PTR(-EBUSY);
		goto out_unlock;
	}
	channel->ops = ops;
	channel->context = context;
out_unlock:
	mutex_unlock(&transport->lifetime_lock);
	return channel;
}

static int open_channel(struct marlin2_channel *channel, bool fresh)
{
	struct marlin2_transport *transport = channel->transport;
	int ret = 0;

	mutex_lock(&transport->lifetime_lock);
	if (channel->active || (fresh && transport->users)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (transport->users && READ_ONCE(transport->io_error)) {
		ret = READ_ONCE(transport->io_error);
		goto out_unlock;
	}
	/* Publish the consumer before first IRQ delivery can begin. */
	smp_store_release(&channel->active, true);
	if (!transport->users) {
		ret = start_transport(transport);
		if (ret) {
			WRITE_ONCE(channel->active, false);
			goto out_unlock;
		}
	}
	transport->users++;
out_unlock:
	mutex_unlock(&transport->lifetime_lock);
	return ret;
}

int marlin2_channel_open(struct marlin2_channel *channel)
{
	return open_channel(channel, false);
}

int marlin2_channel_open_fresh(struct marlin2_channel *channel)
{
	return open_channel(channel, true);
}

static void close_channel_locked(struct marlin2_channel *channel)
{
	struct marlin2_transport *transport = channel->transport;

	if (!channel->active)
		return;
	WRITE_ONCE(channel->active, false);
	transport->users--;
	if (!transport->users)
		stop_transport(transport);
	else {
		mutex_lock(&channel->callback_lock);
		mutex_unlock(&channel->callback_lock);
	}
}

void marlin2_channel_close(struct marlin2_channel *channel)
{
	struct marlin2_transport *transport = channel->transport;

	mutex_lock(&transport->lifetime_lock);
	close_channel_locked(channel);
	mutex_unlock(&transport->lifetime_lock);
}

void marlin2_channel_unregister(struct marlin2_channel *channel)
{
	struct marlin2_transport *transport = channel->transport;

	mutex_lock(&transport->lifetime_lock);
	close_channel_locked(channel);
	channel->ops = NULL;
	channel->context = NULL;
	mutex_unlock(&transport->lifetime_lock);
}

static int channel_io_error(struct marlin2_channel *channel)
{
	struct marlin2_transport *transport = channel->transport;

	if (!READ_ONCE(channel->active) || !READ_ONCE(transport->running))
		return -ENETDOWN;
	return READ_ONCE(transport->io_error);
}

int marlin2_channel_write(struct marlin2_channel *channel, const u8 *packet,
			  size_t bytes)
{
	struct marlin2_transport *transport = channel->transport;
	int ret;

	if (!bytes || bytes > MARLIN2_TRANSPORT_TX_MAX_BYTES || bytes % 512)
		return -EMSGSIZE;
	mutex_lock(&transport->io_lock);
	ret = channel_io_error(channel);
	if (!ret) {
		sdio_claim_host(transport->func);
		ret = sdio_writesb(transport->func,
				   MARLIN2_TRANSPORT_PACKET_PORT,
				   (void *)packet, bytes);
		sdio_release_host(transport->func);
	}
	mutex_unlock(&transport->io_lock);
	return ret;
}

int marlin2_channel_send(struct marlin2_channel *channel, u8 subtype,
			 const u8 *payload, size_t payload_bytes)
{
	u8 *packet;
	size_t bytes;
	int ret;

	packet = kmalloc(MARLIN2_TRANSPORT_TX_MAX_BYTES, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	bytes = marlin2_transport_record_build(packet,
					       MARLIN2_TRANSPORT_TX_MAX_BYTES,
					       channel->client, subtype,
					       payload, payload_bytes);
	ret = bytes ? marlin2_channel_write(channel, packet, bytes) : -EMSGSIZE;
	kfree(packet);
	return ret;
}

int marlin2_channel_write_bluetooth(struct marlin2_channel *channel,
				    const u8 *data, size_t bytes)
{
	struct marlin2_transport *transport = channel->transport;
	int ret;

	if (channel->client != MARLIN2_CLIENT_BLUETOOTH ||
	    !transport->radio_ops->write_bluetooth_locked)
		return -EOPNOTSUPP;
	mutex_lock(&transport->io_lock);
	ret = channel_io_error(channel);
	if (!ret) {
		sdio_claim_host(transport->func);
		ret = transport->radio_ops->write_bluetooth_locked(
			transport->func, data, bytes);
		sdio_release_host(transport->func);
	}
	mutex_unlock(&transport->io_lock);
	return ret;
}
