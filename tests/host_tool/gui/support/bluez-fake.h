/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_BLUEZ_FAKE_H
#define FPLINUX_TEST_BLUEZ_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A test-owned org.bluez on the bus that DBUS_SYSTEM_BUS_ADDRESS names. It
 * answers adapter/device properties and methods, exports device inventory
 * and calls the registered pairing agent. It replaces the D-Bus boundary,
 * without a Bluetooth controller. The owner calls bluez_fake_pump() from
 * its own loop; nothing in it blocks except bluez_fake_start().
 */
struct bluez_fake;

enum bluez_fake_answer {
	/* Applies the request, signals PropertiesChanged, then replies. */
	BLUEZ_FAKE_APPLY,
	BLUEZ_FAKE_FAIL,
	BLUEZ_FAKE_SILENT,
};

struct bluez_fake *bluez_fake_start(void);
/* Leaves the bus as an exiting bluetoothd does. */
void bluez_fake_stop(struct bluez_fake *fake);
void bluez_fake_pump(struct bluez_fake *fake);
void bluez_fake_add_adapter(struct bluez_fake *fake, const char *path,
			    bool powered);
void bluez_fake_remove_adapter(struct bluez_fake *fake, const char *path);
/* A change made by another client, such as bluetoothctl. */
void bluez_fake_set_powered(struct bluez_fake *fake, const char *path,
			    bool powered);
bool bluez_fake_powered(const struct bluez_fake *fake, const char *path);
/* How the following Set requests are answered, after delay_ms. */
void bluez_fake_answer(struct bluez_fake *fake, enum bluez_fake_answer answer,
		       unsigned int delay_ms);
unsigned int bluez_fake_requests(const struct bluez_fake *fake);

enum bluez_fake_agent_kind {
	BLUEZ_FAKE_AGENT_NONE,
	BLUEZ_FAKE_AGENT_PIN,
	BLUEZ_FAKE_AGENT_PASSKEY,
	BLUEZ_FAKE_AGENT_CONFIRMATION,
	BLUEZ_FAKE_AGENT_AUTHORIZATION,
	BLUEZ_FAKE_AGENT_SERVICE,
	BLUEZ_FAKE_AGENT_DISPLAY_PIN,
	BLUEZ_FAKE_AGENT_DISPLAY_PASSKEY,
};

void bluez_fake_add_device(struct bluez_fake *fake, const char *path,
			   const char *address, const char *name, bool paired);
void bluez_fake_remove_device(struct bluez_fake *fake, const char *path);
void bluez_fake_set_device_connected(struct bluez_fake *fake, const char *path,
				     bool connected);
void bluez_fake_set_device_trusted(struct bluez_fake *fake, const char *path,
				   bool trusted);
void bluez_fake_set_device_rssi(struct bluez_fake *fake, const char *path,
				int16_t rssi, bool known);
void bluez_fake_set_device_battery(struct bluez_fake *fake, const char *path,
				   uint8_t battery, bool known);
void bluez_fake_set_device_uuids(struct bluez_fake *fake, const char *path,
				 const char *const *uuids, size_t count);
const char *bluez_fake_device_alias(struct bluez_fake *fake, const char *path);
bool bluez_fake_device_paired(struct bluez_fake *fake, const char *path);
bool bluez_fake_device_trusted(struct bluez_fake *fake, const char *path);
bool bluez_fake_device_connected(struct bluez_fake *fake, const char *path);
bool bluez_fake_device_connection_requested(struct bluez_fake *fake,
					    const char *path,
					    const char *method,
					    const char *uuid);
bool bluez_fake_discovering(struct bluez_fake *fake, const char *path);
bool bluez_fake_discovery_owned(struct bluez_fake *fake, const char *path);
void bluez_fake_set_external_discovery(struct bluez_fake *fake,
				       const char *path, bool discovering);
unsigned int bluez_fake_discovery_requests(const struct bluez_fake *fake);
unsigned int bluez_fake_cancel_requests(const struct bluez_fake *fake);
void bluez_fake_set_pairable(struct bluez_fake *fake, const char *path,
			     bool pairable);
bool bluez_fake_pairable(struct bluez_fake *fake, const char *path);
uint32_t bluez_fake_pairable_timeout(struct bluez_fake *fake, const char *path);
bool bluez_fake_agent_registered(const struct bluez_fake *fake);
void bluez_fake_set_pairing_request(struct bluez_fake *fake,
				    enum bluez_fake_agent_kind kind,
				    uint32_t passkey);
void bluez_fake_request_agent(struct bluez_fake *fake,
			      enum bluez_fake_agent_kind kind, const char *path,
			      uint32_t passkey, uint16_t entered,
			      const char *text);
bool bluez_fake_agent_answered(const struct bluez_fake *fake);
bool bluez_fake_agent_rejected(const struct bluez_fake *fake);
const char *bluez_fake_agent_code(const struct bluez_fake *fake);
/* Calls from an unrelated bus peer, to check Agent1's sender boundary. */
void bluez_fake_forge_agent(struct bluez_fake *fake, const char *device);
void bluez_fake_cancel_agent(struct bluez_fake *fake, bool release);

#endif
