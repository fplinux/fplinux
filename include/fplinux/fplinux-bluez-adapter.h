/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BLUEZ_ADAPTER_H
#define FPLINUX_BLUEZ_ADAPTER_H

#include <stdbool.h>

/*
 * The power switch of the one BlueZ adapter on the system bus. No call
 * waits for the bus: the owner polls fplinux_bluez_adapter_fd() for
 * input and runs fplinux_bluez_adapter_dispatch() when it is readable.
 */
struct fplinux_bluez_adapter;

struct fplinux_bluez_adapter_listener {
	/* The adapter appeared, went away or changed its power. */
	void (*changed)(void *context);
	/* A set_powered() request ended; applied is false when BlueZ refused it,
	 * did not answer in time or went away. */
	void (*finished)(bool applied, void *context);
	void *context;
};

/* NULL with errno when the descriptor or the state cannot be allocated. */
struct fplinux_bluez_adapter *fplinux_bluez_adapter_open(
	const struct fplinux_bluez_adapter_listener *listener);
void fplinux_bluez_adapter_close(struct fplinux_bluez_adapter *adapter);
/* Stays the same descriptor from open to close. */
int fplinux_bluez_adapter_fd(const struct fplinux_bluez_adapter *adapter);
/*
 * Connects to the bus unless connected, for example after the bus daemon
 * restarted. A bus that is absent leaves Bluetooth absent until the next call.
 */
void fplinux_bluez_adapter_connect(struct fplinux_bluez_adapter *adapter);
void fplinux_bluez_adapter_dispatch(struct fplinux_bluez_adapter *adapter);
/* Only while BlueZ reports exactly one adapter. */
bool fplinux_bluez_adapter_present(const struct fplinux_bluez_adapter *adapter);
bool fplinux_bluez_adapter_powered(const struct fplinux_bluez_adapter *adapter);
/* A set_powered() request is waiting for its answer. */
bool fplinux_bluez_adapter_pending(const struct fplinux_bluez_adapter *adapter);
/*
 * Asks BlueZ to switch the adapter, answered through finished() within
 * timeout_ms. False without an adapter, during another request or when the
 * request cannot be queued.
 */
bool fplinux_bluez_adapter_set_powered(struct fplinux_bluez_adapter *adapter,
				       bool powered, int timeout_ms);

#endif
