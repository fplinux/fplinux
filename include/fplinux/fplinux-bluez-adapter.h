/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BLUEZ_ADAPTER_H
#define FPLINUX_BLUEZ_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FPLINUX_BLUEZ_PATH_BYTES 256
#define FPLINUX_BLUEZ_NAME_BYTES 256
#define FPLINUX_BLUEZ_UUID_BYTES 37
#define FPLINUX_BLUEZ_DEVICE_COUNT 64
#define FPLINUX_BLUEZ_UUID_COUNT 32

enum fplinux_bluez_visibility {
	FPLINUX_BLUEZ_HIDDEN,
	FPLINUX_BLUEZ_VISIBLE_TIMED,
	FPLINUX_BLUEZ_VISIBLE_ALWAYS,
};

struct fplinux_bluez_device {
	char path[FPLINUX_BLUEZ_PATH_BYTES];
	char address[18];
	char name[FPLINUX_BLUEZ_NAME_BYTES];
	char alias[FPLINUX_BLUEZ_NAME_BYTES];
	char icon[64];
	bool paired;
	bool trusted;
	bool connected;
	bool services_resolved;
	bool rssi_known;
	int16_t rssi;
	bool battery_known;
	uint8_t battery;
	size_t uuid_count;
	char uuids[FPLINUX_BLUEZ_UUID_COUNT][FPLINUX_BLUEZ_UUID_BYTES];
};

enum fplinux_bluez_agent_kind {
	FPLINUX_BLUEZ_AGENT_NONE,
	FPLINUX_BLUEZ_AGENT_PIN,
	FPLINUX_BLUEZ_AGENT_PASSKEY,
	FPLINUX_BLUEZ_AGENT_CONFIRMATION,
	FPLINUX_BLUEZ_AGENT_AUTHORIZATION,
	FPLINUX_BLUEZ_AGENT_SERVICE,
	FPLINUX_BLUEZ_AGENT_DISPLAY_PIN,
	FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY,
};

struct fplinux_bluez_agent_request {
	uint64_t serial;
	enum fplinux_bluez_agent_kind kind;
	char device[FPLINUX_BLUEZ_PATH_BYTES];
	char pin[17];
	uint32_t passkey;
	uint16_t entered;
	char uuid[FPLINUX_BLUEZ_UUID_BYTES];
};

/*
 * The one BlueZ adapter and its devices on the system bus. No call
 * waits for the bus: the owner polls fplinux_bluez_adapter_fd() for
 * input and runs fplinux_bluez_adapter_dispatch() when it is readable.
 */
struct fplinux_bluez_adapter;

struct fplinux_bluez_adapter_listener {
	/* Adapter, device inventory or pairing request changed. */
	void (*changed)(void *context);
	/* A mutation request ended; applied is false when BlueZ refused it,
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
/* A mutation is waiting for its answer. */
bool fplinux_bluez_adapter_pending(const struct fplinux_bluez_adapter *adapter);
/*
 * Asks BlueZ to switch the adapter, answered through finished() within
 * timeout_ms. False without an adapter, during another request or when the
 * request cannot be queued.
 */
bool fplinux_bluez_adapter_set_powered(struct fplinux_bluez_adapter *adapter,
				       bool powered, int timeout_ms);

const char *
fplinux_bluez_adapter_name(const struct fplinux_bluez_adapter *adapter);
enum fplinux_bluez_visibility
fplinux_bluez_adapter_visibility(const struct fplinux_bluez_adapter *adapter);
bool fplinux_bluez_adapter_discovering(
	const struct fplinux_bluez_adapter *adapter);
bool fplinux_bluez_adapter_discovery_owned(
	const struct fplinux_bluez_adapter *adapter);
/* errno value for the latest failed request, zero after a successful request. */
int fplinux_bluez_adapter_error(const struct fplinux_bluez_adapter *adapter);
size_t
fplinux_bluez_adapter_device_count(const struct fplinux_bluez_adapter *adapter);
/* Borrowed until the next dispatch; retain path, never an inventory index. */
const struct fplinux_bluez_device *
fplinux_bluez_adapter_device_at(const struct fplinux_bluez_adapter *adapter,
				size_t index);
const struct fplinux_bluez_device *
fplinux_bluez_adapter_device(const struct fplinux_bluez_adapter *adapter,
			     const char *path);

/* A single mutation may be pending. False sets error() immediately. */
bool fplinux_bluez_adapter_set_name(struct fplinux_bluez_adapter *adapter,
				    const char *name, int timeout_ms);
/* Visible modes open incoming pairing for the same duration. Hidden only
 * changes discoverability, preserving the current incoming-pairing policy. */
bool fplinux_bluez_adapter_set_visibility(
	struct fplinux_bluez_adapter *adapter,
	enum fplinux_bluez_visibility visibility, int timeout_ms);
/* Stop releases only a discovery session acquired by this client. */
bool fplinux_bluez_adapter_set_discovery(struct fplinux_bluez_adapter *adapter,
					 bool discover, int timeout_ms);
bool fplinux_bluez_adapter_pair(struct fplinux_bluez_adapter *adapter,
				const char *path, int timeout_ms);
bool fplinux_bluez_adapter_connect_device(struct fplinux_bluez_adapter *adapter,
					  const char *path, int timeout_ms);
bool fplinux_bluez_adapter_disconnect_device(
	struct fplinux_bluez_adapter *adapter, const char *path,
	int timeout_ms);
bool fplinux_bluez_adapter_connect_profile(struct fplinux_bluez_adapter *adapter,
					   const char *path, const char *uuid,
					   int timeout_ms);
bool fplinux_bluez_adapter_set_trusted(struct fplinux_bluez_adapter *adapter,
				       const char *path, bool trusted,
				       int timeout_ms);
bool fplinux_bluez_adapter_set_alias(struct fplinux_bluez_adapter *adapter,
				     const char *path, const char *alias,
				     int timeout_ms);
bool fplinux_bluez_adapter_remove_device(struct fplinux_bluez_adapter *adapter,
					 const char *path, int timeout_ms);
/* Cancel a pending Pair, including any displayed request. */
bool fplinux_bluez_adapter_cancel_pairing(struct fplinux_bluez_adapter *adapter);

/* Enable only while a UI can answer prompts. Also requests default-agent duty. */
bool fplinux_bluez_adapter_set_agent_enabled(
	struct fplinux_bluez_adapter *adapter, bool enabled);
bool fplinux_bluez_adapter_agent_ready(
	const struct fplinux_bluez_adapter *adapter);
const struct fplinux_bluez_agent_request *fplinux_bluez_adapter_agent_request(
	const struct fplinux_bluez_adapter *adapter);
/* serial rejects stale replies. PIN is 1..16 characters, passkey 0..999999. */
bool fplinux_bluez_adapter_agent_reply(struct fplinux_bluez_adapter *adapter,
				       uint64_t serial, bool allow,
				       const char *code);

#endif
