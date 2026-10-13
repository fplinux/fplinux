/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_TRANSPORT_H
#define FPLINUX_MARLIN2_TRANSPORT_H

#include <linux/types.h>

struct device;
struct gpio_desc;
struct sdio_func;
struct marlin2_channel;
struct marlin2_transport;

enum marlin2_client {
	MARLIN2_CLIENT_BLUETOOTH,
	MARLIN2_CLIENT_FM,
	MARLIN2_CLIENT_WLAN,
	MARLIN2_CLIENT_COUNT,
};

/*
 * acquire() returns the existing board boot's enabled, awake function 1 and
 * packet request input. The last close() stops IRQ delivery and SDIO writes
 * before release(). The owner grants no firmware sleep permission.
 */
struct marlin2_transport_radio_ops {
	int (*acquire)(void *context, struct sdio_func **func,
		       struct gpio_desc **rx_request);
	void (*release)(void *context);
	int (*write_bluetooth_locked)(struct sdio_func *func, const u8 *data,
				      size_t bytes);
};

/*
 * Callbacks run in the packet IRQ thread. Payload storage is borrowed only
 * during receive(). They may send but must not open, close or unregister a
 * channel. close() waits for callbacks; consumers stop their transmit work
 * before close() and release their callback state after unregister().
 */
struct marlin2_channel_ops {
	void (*receive)(void *context, u8 subtype, const u8 *data,
			size_t bytes);
	void (*error)(void *context, int error);
};

struct marlin2_transport *
marlin2_transport_create(struct device *dev,
			 const struct marlin2_transport_radio_ops *radio_ops,
			 void *radio_context);
/* Every consumer must unregister its channel before the owner is destroyed. */
void marlin2_transport_destroy(struct marlin2_transport *transport);
struct marlin2_channel *
marlin2_channel_register(struct marlin2_transport *transport,
			 enum marlin2_client client,
			 const struct marlin2_channel_ops *ops, void *context);
void marlin2_channel_unregister(struct marlin2_channel *channel);
int marlin2_channel_open(struct marlin2_channel *channel);
/* Requires no active consumers; success includes a new board boot acquisition. */
int marlin2_channel_open_fresh(struct marlin2_channel *channel);
void marlin2_channel_close(struct marlin2_channel *channel);
int marlin2_channel_send(struct marlin2_channel *channel, u8 subtype,
			 const u8 *payload, size_t payload_bytes);
/* WLAN may retain its current aggregate builder during adapter integration. */
int marlin2_channel_write(struct marlin2_channel *channel, const u8 *packet,
			  size_t bytes);
/* H4 command and ACL data use the controller's direct write path. */
int marlin2_channel_write_bluetooth(struct marlin2_channel *channel,
				    const u8 *data, size_t bytes);

#endif
