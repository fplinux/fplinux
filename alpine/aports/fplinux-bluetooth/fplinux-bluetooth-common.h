// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BLUETOOTH_COMMON_H
#define FPLINUX_BLUETOOTH_COMMON_H

#include <dbus/dbus.h>
#include <stdint.h>

int fplinux_bluetooth_fail(const char *format, ...);
int64_t fplinux_bluetooth_now_ms(void);
DBusConnection *fplinux_bluetooth_system_bus(void);
DBusMessage *fplinux_bluetooth_call(DBusConnection *connection,
				    DBusMessage *message, int timeout_ms);

#endif
