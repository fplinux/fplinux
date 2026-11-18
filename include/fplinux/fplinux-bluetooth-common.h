// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BLUETOOTH_COMMON_H
#define FPLINUX_BLUETOOTH_COMMON_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdint.h>

int fplinux_bluetooth_fail(const char *format, ...);
int64_t fplinux_bluetooth_now_ms(void);
DBusConnection *fplinux_bluetooth_system_bus(void);
/* Private connections must be released with close_bus. The system_bus helper
 * retains libdbus's shared-connection ownership contract. */
DBusConnection *fplinux_bluetooth_private_bus(DBusError *error);
void fplinux_bluetooth_close_bus(DBusConnection *connection);
/* Consumes message. The returned reply is owned by the caller. */
DBusMessage *fplinux_bluetooth_call_cancellable(DBusConnection *connection,
						DBusMessage *message,
						int timeout_ms,
						bool (*cancelled)(void *data),
						void *data, DBusError *error);
DBusMessage *fplinux_bluetooth_call(DBusConnection *connection,
				    DBusMessage *message, int timeout_ms);

#endif
