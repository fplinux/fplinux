/* SPDX-License-Identifier: GPL-2.0-only */
/* The shared BlueZ adapter client against a test-owned org.bluez. */
#define _GNU_SOURCE
#include "bluez-fake.h"
#include "fplinux-bluez-adapter.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HCI0 "/org/bluez/hci0"
#define HCI1 "/org/bluez/hci1"
#define DEVICE HCI0 "/dev_10_20_30_40_50_60"

struct observation {
	unsigned int changes;
	unsigned int answers;
	bool applied;
};

struct driver {
	struct fplinux_bluez_adapter *adapter;
	struct bluez_fake *fake;
	struct observation seen;
	/* Times the descriptor was readable since the counter was reset. */
	unsigned int wakeups;
};

static uint64_t now_ms(void)
{
	struct timespec now;

	assert(!clock_gettime(CLOCK_MONOTONIC, &now));
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void changed(void *context)
{
	struct observation *seen = context;

	++seen->changes;
}

static void finished(bool applied, void *context)
{
	struct observation *seen = context;

	++seen->answers;
	seen->applied = applied;
}

static void step(struct driver *driver, int timeout_ms)
{
	struct pollfd descriptor = {
		.fd = fplinux_bluez_adapter_fd(driver->adapter),
		.events = POLLIN,
	};

	if (driver->fake)
		bluez_fake_pump(driver->fake);
	if (poll(&descriptor, 1, timeout_ms) > 0) {
		++driver->wakeups;
		fplinux_bluez_adapter_dispatch(driver->adapter);
	}
	if (driver->fake)
		bluez_fake_pump(driver->fake);
}

static void run_for(struct driver *driver, unsigned int duration_ms)
{
	uint64_t end = now_ms() + duration_ms;

	while (now_ms() < end)
		step(driver, 5);
}

static bool reaches(struct driver *driver, bool present, bool powered,
		    unsigned int deadline_ms)
{
	uint64_t end = now_ms() + deadline_ms;

	do {
		if (fplinux_bluez_adapter_present(driver->adapter) == present &&
		    fplinux_bluez_adapter_powered(driver->adapter) == powered)
			return true;
		step(driver, 5);
	} while (now_ms() < end);
	return false;
}

static void expect_state(struct driver *driver, bool present, bool powered)
{
	if (!reaches(driver, present, powered, 2000)) {
		fprintf(stderr, "Bluetooth state: present=%d powered=%d\n",
			fplinux_bluez_adapter_present(driver->adapter),
			fplinux_bluez_adapter_powered(driver->adapter));
		assert(false);
	}
}

/* Waits for the answer of one request; returns its time in ms. */
static uint64_t expect_answer(struct driver *driver, bool applied,
			      unsigned int deadline_ms)
{
	unsigned int answers = driver->seen.answers;
	uint64_t start = now_ms();

	while (driver->seen.answers == answers &&
	       now_ms() < start + deadline_ms)
		step(driver, 5);
	assert(driver->seen.answers == answers + 1);
	assert(driver->seen.applied == applied);
	assert(!fplinux_bluez_adapter_pending(driver->adapter));
	return now_ms() - start;
}

/* Nothing left to read: an idle client is not woken up again and again. */
static void expect_idle(struct driver *driver)
{
	run_for(driver, 100);
	driver->wakeups = 0;
	run_for(driver, 300);
	assert(driver->wakeups == 0);
}

static void open_driver(struct driver *driver)
{
	const struct fplinux_bluez_adapter_listener listener = {
		.changed = changed,
		.finished = finished,
		.context = &driver->seen,
	};

	driver->adapter = fplinux_bluez_adapter_open(&listener);
	assert(driver->adapter);
	assert(fplinux_bluez_adapter_fd(driver->adapter) >= 0);
}

static void no_bus(void)
{
	struct driver driver = { 0 };

	open_driver(&driver);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, false, false);
	assert(!fplinux_bluez_adapter_set_powered(driver.adapter, true, 1000));
	expect_idle(&driver);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_idle(&driver);
	assert(!driver.seen.changes && !driver.seen.answers);
	fplinux_bluez_adapter_close(driver.adapter);
	puts("bluez-adapter: no bus leaves Bluetooth absent and idle");
}

static void lifecycle(void)
{
	struct driver driver = { 0 };
	unsigned int changes;

	open_driver(&driver);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, false, false);
	expect_idle(&driver);

	driver.fake = bluez_fake_start();
	expect_idle(&driver);
	assert(!fplinux_bluez_adapter_present(driver.adapter));
	bluez_fake_add_adapter(driver.fake, HCI0, false);
	expect_state(&driver, true, false);

	/* Another client switches it on: the state follows live. */
	changes = driver.seen.changes;
	bluez_fake_set_powered(driver.fake, HCI0, true);
	expect_state(&driver, true, true);
	assert(driver.seen.changes == changes + 1);

	assert(fplinux_bluez_adapter_set_powered(driver.adapter, false, 10000));
	assert(fplinux_bluez_adapter_pending(driver.adapter));
	assert(!fplinux_bluez_adapter_set_powered(driver.adapter, true, 10000));
	expect_answer(&driver, true, 2000);
	assert(!fplinux_bluez_adapter_powered(driver.adapter));
	assert(!bluez_fake_powered(driver.fake, HCI0));
	assert(bluez_fake_requests(driver.fake) == 1);

	bluez_fake_answer(driver.fake, BLUEZ_FAKE_FAIL, 0);
	assert(fplinux_bluez_adapter_set_powered(driver.adapter, true, 10000));
	expect_answer(&driver, false, 2000);
	expect_state(&driver, true, false);

	/* Unanswered within its timeout, while the loop keeps running. */
	bluez_fake_answer(driver.fake, BLUEZ_FAKE_SILENT, 0);
	assert(fplinux_bluez_adapter_set_powered(driver.adapter, true, 300));
	assert(expect_answer(&driver, false, 2000) >= 250);
	expect_state(&driver, true, false);

	bluez_fake_add_adapter(driver.fake, HCI1, true);
	expect_state(&driver, false, false);
	assert(!fplinux_bluez_adapter_set_powered(driver.adapter, true, 1000));
	bluez_fake_remove_adapter(driver.fake, HCI1);
	expect_state(&driver, true, false);
	bluez_fake_remove_adapter(driver.fake, HCI0);
	expect_state(&driver, false, false);
	bluez_fake_add_adapter(driver.fake, HCI0, false);
	expect_state(&driver, true, false);

	/* bluetoothd exits with a request outstanding, then starts again. */
	assert(fplinux_bluez_adapter_set_powered(driver.adapter, true, 10000));
	bluez_fake_stop(driver.fake);
	driver.fake = NULL;
	assert(expect_answer(&driver, false, 2000) < 1500);
	expect_state(&driver, false, false);
	expect_idle(&driver);
	driver.fake = bluez_fake_start();
	bluez_fake_answer(driver.fake, BLUEZ_FAKE_APPLY, 0);
	bluez_fake_add_adapter(driver.fake, HCI0, true);
	expect_state(&driver, true, true);
	expect_idle(&driver);

	fplinux_bluez_adapter_close(driver.adapter);
	bluez_fake_stop(driver.fake);
	puts("bluez-adapter: one adapter switches, follows other clients and bluetoothd restarts");
}

/* The test stops the bus daemon between the two lines. */
static void bus_loss(void)
{
	struct driver driver = { 0 };
	char line[16];

	open_driver(&driver);
	driver.fake = bluez_fake_start();
	bluez_fake_add_adapter(driver.fake, HCI0, true);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, true, true);
	puts("bluez-adapter: connected");
	fflush(stdout);
	assert(fgets(line, sizeof(line), stdin) && !strcmp(line, "go\n"));
	bluez_fake_stop(driver.fake);
	driver.fake = NULL;
	expect_state(&driver, false, false);
	expect_idle(&driver);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, false, false);
	expect_idle(&driver);
	fplinux_bluez_adapter_close(driver.adapter);
	puts("bluez-adapter: a lost bus leaves Bluetooth absent and idle");
}

static const struct fplinux_bluez_device *expect_device(struct driver *driver,
							const char *path)
{
	uint64_t end = now_ms() + 2000;
	const struct fplinux_bluez_device *device;

	while (!(device =
			 fplinux_bluez_adapter_device(driver->adapter, path)) &&
	       now_ms() < end)
		step(driver, 5);
	assert(device);
	return device;
}

static const struct fplinux_bluez_agent_request *
expect_agent(struct driver *driver, enum fplinux_bluez_agent_kind kind)
{
	uint64_t end = now_ms() + 2000;
	const struct fplinux_bluez_agent_request *request;

	do {
		request = fplinux_bluez_adapter_agent_request(driver->adapter);
		if (request->kind == kind)
			return request;
		step(driver, 5);
	} while (now_ms() < end);
	assert(false);
	return NULL;
}

static void expect_agent_answer(struct driver *driver, bool rejected)
{
	uint64_t end = now_ms() + 2000;

	while (!bluez_fake_agent_answered(driver->fake) && now_ms() < end)
		step(driver, 5);
	assert(bluez_fake_agent_answered(driver->fake));
	assert(bluez_fake_agent_rejected(driver->fake) == rejected);
}

static void settings_inventory(void)
{
	struct driver driver = { 0 };
	const struct fplinux_bluez_device *device;
	const char *const uuids[] = {
		"00001105-0000-1000-8000-00805f9b34fb",
		"0000110b-0000-1000-8000-00805f9b34fb",
	};

	open_driver(&driver);
	driver.fake = bluez_fake_start();
	bluez_fake_add_adapter(driver.fake, HCI0, true);
	bluez_fake_add_device(driver.fake, DEVICE, "10:20:30:40:50:60",
			      "Headphones", true);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, true, true);
	device = expect_device(&driver, DEVICE);
	assert(device->paired && !device->trusted && !device->connected);
	assert(!strcmp(device->address, "10:20:30:40:50:60"));
	assert(!strcmp(device->name, "Headphones") &&
	       !strcmp(device->alias, "Headphones"));
	assert(!device->rssi_known && !device->battery_known);
	assert(!strcmp(fplinux_bluez_adapter_name(driver.adapter),
		       "Test adapter"));
	assert(fplinux_bluez_adapter_device_count(driver.adapter) == 1);
	assert(!fplinux_bluez_adapter_device_at(driver.adapter, 1));
	assert(!bluez_fake_agent_registered(driver.fake));

	bluez_fake_set_device_rssi(driver.fake, DEVICE, -54, true);
	bluez_fake_set_device_battery(driver.fake, DEVICE, 77, true);
	bluez_fake_set_device_uuids(driver.fake, DEVICE, uuids, 2);
	run_for(&driver, 100);
	device = expect_device(&driver, DEVICE);
	assert(device->rssi_known && device->rssi == -54);
	assert(device->battery_known && device->battery == 77);
	assert(device->uuid_count == 2 && !strcmp(device->uuids[0], uuids[0]));
	/* A partial change must not reset the other properties of this device. */
	bluez_fake_set_device_connected(driver.fake, DEVICE, true);
	run_for(&driver, 100);
	device = expect_device(&driver, DEVICE);
	assert(device->connected && device->paired && device->uuid_count == 2);
	assert(device->battery_known && device->rssi_known);
	bluez_fake_set_device_rssi(driver.fake, DEVICE, 0, false);
	bluez_fake_set_device_battery(driver.fake, DEVICE, 0, false);
	run_for(&driver, 100);
	device = expect_device(&driver, DEVICE);
	assert(!device->rssi_known && !device->battery_known && device->paired);

	assert(fplinux_bluez_adapter_set_name(driver.adapter, "Pocket", 1000));
	expect_answer(&driver, true, 2000);
	assert(!strcmp(fplinux_bluez_adapter_name(driver.adapter), "Pocket"));
	assert(!bluez_fake_pairable(driver.fake, HCI0));
	assert(fplinux_bluez_adapter_set_visibility(
		driver.adapter, FPLINUX_BLUEZ_VISIBLE_TIMED, 1000));
	expect_answer(&driver, true, 2000);
	assert(fplinux_bluez_adapter_visibility(driver.adapter) ==
	       FPLINUX_BLUEZ_VISIBLE_TIMED);
	assert(bluez_fake_pairable(driver.fake, HCI0));
	assert(bluez_fake_pairable_timeout(driver.fake, HCI0) == 120);
	/* The pairing timer or another client can close the window while visible. */
	bluez_fake_set_pairable(driver.fake, HCI0, false);
	assert(fplinux_bluez_adapter_set_visibility(
		driver.adapter, FPLINUX_BLUEZ_VISIBLE_TIMED, 1000));
	expect_answer(&driver, true, 2000);
	assert(bluez_fake_pairable(driver.fake, HCI0));
	assert(bluez_fake_pairable_timeout(driver.fake, HCI0) == 120);
	assert(fplinux_bluez_adapter_set_visibility(
		driver.adapter, FPLINUX_BLUEZ_VISIBLE_ALWAYS, 1000));
	expect_answer(&driver, true, 2000);
	assert(fplinux_bluez_adapter_visibility(driver.adapter) ==
	       FPLINUX_BLUEZ_VISIBLE_ALWAYS);
	assert(bluez_fake_pairable(driver.fake, HCI0));
	assert(bluez_fake_pairable_timeout(driver.fake, HCI0) == 0);
	assert(fplinux_bluez_adapter_set_visibility(
		driver.adapter, FPLINUX_BLUEZ_HIDDEN, 1000));
	expect_answer(&driver, true, 2000);
	assert(fplinux_bluez_adapter_visibility(driver.adapter) ==
	       FPLINUX_BLUEZ_HIDDEN);
	assert(bluez_fake_pairable(driver.fake, HCI0));
	bluez_fake_set_pairable(driver.fake, HCI0, false);
	assert(fplinux_bluez_adapter_set_visibility(
		driver.adapter, FPLINUX_BLUEZ_HIDDEN, 1000));
	expect_answer(&driver, true, 2000);
	assert(!bluez_fake_pairable(driver.fake, HCI0));

	bluez_fake_set_external_discovery(driver.fake, HCI0, true);
	run_for(&driver, 100);
	assert(fplinux_bluez_adapter_discovering(driver.adapter));
	assert(!fplinux_bluez_adapter_set_discovery(driver.adapter, false,
						    1000));
	assert(bluez_fake_discovery_requests(driver.fake) == 0);
	assert(fplinux_bluez_adapter_set_discovery(driver.adapter, true, 1000));
	expect_answer(&driver, true, 2000);
	assert(fplinux_bluez_adapter_discovery_owned(driver.adapter));
	assert(fplinux_bluez_adapter_set_discovery(driver.adapter, false,
						   1000));
	expect_answer(&driver, true, 2000);
	assert(!fplinux_bluez_adapter_discovery_owned(driver.adapter));
	assert(bluez_fake_discovering(driver.fake, HCI0));
	assert(fplinux_bluez_adapter_discovering(driver.adapter));
	assert(bluez_fake_discovery_requests(driver.fake) == 2);

	assert(fplinux_bluez_adapter_set_alias(driver.adapter, DEVICE, "Desk",
					       1000));
	expect_answer(&driver, true, 2000);
	assert(!strcmp(bluez_fake_device_alias(driver.fake, DEVICE), "Desk"));
	assert(fplinux_bluez_adapter_set_trusted(driver.adapter, DEVICE, true,
						 1000));
	expect_answer(&driver, true, 2000);
	assert(bluez_fake_device_trusted(driver.fake, DEVICE));
	assert(fplinux_bluez_adapter_disconnect_device(driver.adapter, DEVICE,
						       1000));
	expect_answer(&driver, true, 2000);
	assert(!bluez_fake_device_connected(driver.fake, DEVICE));
	assert(fplinux_bluez_adapter_connect_profile(driver.adapter, DEVICE,
						     uuids[1], 1000));
	expect_answer(&driver, true, 2000);
	assert(bluez_fake_device_connection_requested(
		driver.fake, DEVICE, "ConnectProfile", uuids[1]));
	assert(bluez_fake_device_connected(driver.fake, DEVICE));
	assert(fplinux_bluez_adapter_remove_device(driver.adapter, DEVICE,
						   1000));
	expect_answer(&driver, true, 2000);
	run_for(&driver, 100);
	assert(!fplinux_bluez_adapter_device(driver.adapter, DEVICE));
	assert(!fplinux_bluez_adapter_connect_device(driver.adapter, DEVICE,
						     1000));
	assert(fplinux_bluez_adapter_error(driver.adapter) == ENODEV);
	fplinux_bluez_adapter_close(driver.adapter);
	bluez_fake_stop(driver.fake);
	puts("bluez-adapter: real inventory properties and owned discovery sessions");
}

static void pairing_agent(void)
{
	struct driver driver = { 0 };
	const struct fplinux_bluez_agent_request *request;
	uint64_t serial, end;

	open_driver(&driver);
	driver.fake = bluez_fake_start();
	bluez_fake_add_adapter(driver.fake, HCI0, true);
	bluez_fake_add_device(driver.fake, DEVICE, "10:20:30:40:50:60",
			      "Keyboard", false);
	fplinux_bluez_adapter_connect(driver.adapter);
	expect_state(&driver, true, true);
	expect_device(&driver, DEVICE);
	assert(!bluez_fake_agent_registered(driver.fake));
	assert(fplinux_bluez_adapter_set_agent_enabled(driver.adapter, true));
	end = now_ms() + 2000;
	while (!fplinux_bluez_adapter_agent_ready(driver.adapter) &&
	       now_ms() < end)
		step(&driver, 5);
	assert(fplinux_bluez_adapter_agent_ready(driver.adapter));

	bluez_fake_forge_agent(driver.fake, DEVICE);
	expect_agent_answer(&driver, true);
	assert(fplinux_bluez_adapter_agent_request(driver.adapter)->kind ==
	       FPLINUX_BLUEZ_AGENT_NONE);
	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_PIN, DEVICE, 0,
				 0, NULL);
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_PIN);
	serial = request->serial;
	assert(!fplinux_bluez_adapter_agent_reply(driver.adapter, serial + 1,
						  true, "1234"));
	assert(!fplinux_bluez_adapter_agent_reply(driver.adapter, serial, true,
						  ""));
	assert(!bluez_fake_agent_answered(driver.fake));
	assert(fplinux_bluez_adapter_agent_reply(driver.adapter, serial, true,
						 "42AB"));
	expect_agent_answer(&driver, false);
	assert(!strcmp(bluez_fake_agent_code(driver.fake), "42AB"));

	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_PASSKEY, DEVICE,
				 0, 0, NULL);
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_PASSKEY);
	assert(!fplinux_bluez_adapter_agent_reply(
		driver.adapter, request->serial, true, "12A"));
	assert(fplinux_bluez_adapter_agent_reply(
		driver.adapter, request->serial, true, "000123"));
	expect_agent_answer(&driver, false);
	assert(!strcmp(bluez_fake_agent_code(driver.fake), "000123"));

	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_AUTHORIZATION,
				 DEVICE, 0, 0, NULL);
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_AUTHORIZATION);
	assert(fplinux_bluez_adapter_agent_reply(driver.adapter,
						 request->serial, false, NULL));
	expect_agent_answer(&driver, true);
	assert(!bluez_fake_device_paired(driver.fake, DEVICE));

	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_SERVICE, DEVICE,
				 0, 0, "00001105-0000-1000-8000-00805f9b34fb");
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_SERVICE);
	assert(!strcmp(request->uuid, "00001105-0000-1000-8000-00805f9b34fb"));
	assert(fplinux_bluez_adapter_agent_reply(driver.adapter,
						 request->serial, true, NULL));
	expect_agent_answer(&driver, false);

	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_DISPLAY_PASSKEY,
				 DEVICE, 4271, 3, NULL);
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY);
	assert(request->passkey == 4271 && request->entered == 3);
	assert(!fplinux_bluez_adapter_agent_reply(driver.adapter,
						  request->serial, true, NULL));
	expect_agent_answer(&driver, false);
	assert(!bluez_fake_device_paired(driver.fake, DEVICE));
	bluez_fake_cancel_agent(driver.fake, false);
	expect_agent(&driver, FPLINUX_BLUEZ_AGENT_NONE);
	bluez_fake_request_agent(driver.fake, BLUEZ_FAKE_AGENT_DISPLAY_PIN,
				 DEVICE, 0, 0, "004271");
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_DISPLAY_PIN);
	assert(!strcmp(request->pin, "004271"));
	expect_agent_answer(&driver, false);
	bluez_fake_cancel_agent(driver.fake, false);
	expect_agent(&driver, FPLINUX_BLUEZ_AGENT_NONE);

	bluez_fake_set_pairing_request(driver.fake,
				       BLUEZ_FAKE_AGENT_CONFIRMATION, 123456);
	assert(fplinux_bluez_adapter_pair(driver.adapter, DEVICE, 2000));
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_CONFIRMATION);
	assert(request->passkey == 123456);
	assert(fplinux_bluez_adapter_pending(driver.adapter));
	assert(!bluez_fake_device_paired(driver.fake, DEVICE));
	assert(fplinux_bluez_adapter_agent_reply(driver.adapter,
						 request->serial, true, NULL));
	expect_answer(&driver, true, 2000);
	assert(bluez_fake_device_paired(driver.fake, DEVICE));

	assert(fplinux_bluez_adapter_pair(driver.adapter, DEVICE, 2000));
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_CONFIRMATION);
	serial = request->serial;
	bluez_fake_cancel_agent(driver.fake, true);
	expect_answer(&driver, false, 2000);
	assert(fplinux_bluez_adapter_error(driver.adapter) == ECANCELED);
	assert(!fplinux_bluez_adapter_agent_ready(driver.adapter));
	assert(!fplinux_bluez_adapter_agent_reply(driver.adapter, serial, true,
						  NULL));
	/* A new owner cannot inherit a previous owner's prompt or device state. */
	assert(fplinux_bluez_adapter_set_agent_enabled(driver.adapter, true));
	end = now_ms() + 2000;
	while (!fplinux_bluez_adapter_agent_ready(driver.adapter) &&
	       now_ms() < end)
		step(&driver, 5);
	assert(fplinux_bluez_adapter_agent_ready(driver.adapter));
	assert(fplinux_bluez_adapter_pair(driver.adapter, DEVICE, 2000));
	request = expect_agent(&driver, FPLINUX_BLUEZ_AGENT_CONFIRMATION);
	serial = request->serial;
	bluez_fake_stop(driver.fake);
	driver.fake = NULL;
	expect_answer(&driver, false, 2000);
	expect_state(&driver, false, false);
	assert(!fplinux_bluez_adapter_device_count(driver.adapter));
	assert(!fplinux_bluez_adapter_agent_reply(driver.adapter, serial, true,
						  NULL));
	driver.fake = bluez_fake_start();
	bluez_fake_add_adapter(driver.fake, HCI0, true);
	expect_state(&driver, true, true);
	end = now_ms() + 2000;
	while (!fplinux_bluez_adapter_agent_ready(driver.adapter) &&
	       now_ms() < end)
		step(&driver, 5);
	assert(fplinux_bluez_adapter_agent_ready(driver.adapter));
	assert(!fplinux_bluez_adapter_device_count(driver.adapter));
	assert(fplinux_bluez_adapter_agent_request(driver.adapter)->kind ==
	       FPLINUX_BLUEZ_AGENT_NONE);
	/* Leaving the UI must cancel a real Pair even before an agent prompt. */
	bluez_fake_add_device(driver.fake, DEVICE, "10:20:30:40:50:60",
			      "Keyboard", false);
	expect_device(&driver, DEVICE);
	bluez_fake_answer(driver.fake, BLUEZ_FAKE_SILENT, 0);
	assert(fplinux_bluez_adapter_pair(driver.adapter, DEVICE, 2000));
	run_for(&driver, 100);
	assert(fplinux_bluez_adapter_pending(driver.adapter));
	{
		unsigned int answers = driver.seen.answers;

		assert(fplinux_bluez_adapter_set_agent_enabled(driver.adapter,
							       false));
		assert(driver.seen.answers == answers + 1 &&
		       !driver.seen.applied);
	}
	run_for(&driver, 100);
	assert(!fplinux_bluez_adapter_pending(driver.adapter));
	assert(bluez_fake_cancel_requests(driver.fake) == 1);
	fplinux_bluez_adapter_close(driver.adapter);
	bluez_fake_stop(driver.fake);
	puts("bluez-adapter: authenticated pairing requests require real user replies");
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "no-bus"))
		no_bus();
	else if (!strcmp(argv[1], "lifecycle"))
		lifecycle();
	else if (!strcmp(argv[1], "bus-loss"))
		bus_loss();
	else if (!strcmp(argv[1], "settings-inventory"))
		settings_inventory();
	else if (!strcmp(argv[1], "pairing-agent"))
		pairing_agent();
	else
		return 2;
	return 0;
}
