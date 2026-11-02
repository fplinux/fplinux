/* SPDX-License-Identifier: GPL-2.0-only */
/* The shared BlueZ adapter client against a test-owned org.bluez. */
#define _GNU_SOURCE
#include "bluez-fake.h"
#include "fplinux-bluez-adapter.h"

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HCI0 "/org/bluez/hci0"
#define HCI1 "/org/bluez/hci1"

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

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "no-bus"))
		no_bus();
	else if (!strcmp(argv[1], "lifecycle"))
		lifecycle();
	else if (!strcmp(argv[1], "bus-loss"))
		bus_loss();
	else
		return 2;
	return 0;
}
