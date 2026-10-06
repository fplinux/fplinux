// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "fplinux-bluetooth-common.h"

int fplinux_bluetooth_fail(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	fputs("fplinux-bluetooth: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
	return 1;
}

int64_t fplinux_bluetooth_now_ms(void)
{
	struct timespec value;
	if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
		return -1;
	return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

DBusConnection *fplinux_bluetooth_system_bus(void)
{
	DBusError error;
	dbus_error_init(&error);
	DBusConnection *connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		fplinux_bluetooth_fail("cannot connect to system D-Bus: %s",
				       error.message ?: "unknown error");
		dbus_error_free(&error);
	}
	return connection;
}

DBusMessage *fplinux_bluetooth_call(DBusConnection *connection,
				    DBusMessage *message, int timeout_ms)
{
	DBusError error;
	dbus_error_init(&error);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(
		connection, message, timeout_ms, &error);
	dbus_message_unref(message);
	if (reply == NULL) {
		fplinux_bluetooth_fail("D-Bus call failed: %s",
				       error.message ?: "unknown error");
		dbus_error_free(&error);
	}
	return reply;
}
