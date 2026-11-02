/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_BLUEZ_FAKE_H
#define FPLINUX_TEST_BLUEZ_FAKE_H

#include <stdbool.h>

/*
 * A test-owned org.bluez on the bus that DBUS_SYSTEM_BUS_ADDRESS names. It
 * answers ObjectManager.GetManagedObjects and Properties.Set(Powered) of
 * org.bluez.Adapter1 objects and emits the signals BlueZ sends for them;
 * it models nothing else of BlueZ. The owner calls bluez_fake_pump() from
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

#endif
