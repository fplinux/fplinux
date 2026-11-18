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
	if (!dbus_threads_init_default()) {
		fplinux_bluetooth_fail("cannot initialize D-Bus threading");
		return NULL;
	}
	DBusConnection *connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		fplinux_bluetooth_fail("cannot connect to system D-Bus: %s",
				       error.message ?: "unknown error");
		dbus_error_free(&error);
	}
	return connection;
}

DBusConnection *fplinux_bluetooth_private_bus(DBusError *error)
{
	if (!dbus_threads_init_default()) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY,
				     "cannot initialize D-Bus threading");
		return NULL;
	}
	DBusConnection *connection =
		dbus_bus_get_private(DBUS_BUS_SYSTEM, error);
	if (connection != NULL)
		dbus_connection_set_exit_on_disconnect(connection, false);
	return connection;
}

void fplinux_bluetooth_close_bus(DBusConnection *connection)
{
	if (connection == NULL)
		return;
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
}

DBusMessage *fplinux_bluetooth_call_cancellable(DBusConnection *connection,
						DBusMessage *message,
						int timeout_ms,
						bool (*cancelled)(void *data),
						void *data, DBusError *error)
{
	DBusPendingCall *pending = NULL;
	DBusMessage *reply = NULL;
	int64_t deadline = fplinux_bluetooth_now_ms() + timeout_ms;

	if (cancelled != NULL && cancelled(data)) {
		dbus_set_error_const(error, DBUS_ERROR_FAILED, "interrupted");
		goto out;
	}
	if (!dbus_connection_send_with_reply(connection, message, &pending,
					     timeout_ms) ||
	    pending == NULL) {
		dbus_set_error_const(error, DBUS_ERROR_NO_MEMORY,
				     "cannot queue D-Bus call");
		goto out;
	}
	while (!dbus_pending_call_get_completed(pending)) {
		if (cancelled != NULL && cancelled(data)) {
			dbus_set_error_const(error, DBUS_ERROR_FAILED,
					     "interrupted");
			goto out;
		}
		int64_t remaining = deadline - fplinux_bluetooth_now_ms();
		if (remaining <= 0) {
			dbus_set_error_const(error, DBUS_ERROR_NO_REPLY,
					     "D-Bus call timed out");
			goto out;
		}
		if (!dbus_connection_read_write_dispatch(
			    connection, remaining > 50 ? 50 : (int)remaining)) {
			dbus_set_error_const(error, DBUS_ERROR_DISCONNECTED,
					     "system D-Bus disconnected");
			goto out;
		}
	}
	reply = dbus_pending_call_steal_reply(pending);
	if (reply != NULL && dbus_set_error_from_message(error, reply)) {
		dbus_message_unref(reply);
		reply = NULL;
	}
out:
	if (pending != NULL) {
		dbus_pending_call_cancel(pending);
		dbus_pending_call_unref(pending);
	}
	dbus_message_unref(message);
	return reply;
}

DBusMessage *fplinux_bluetooth_call(DBusConnection *connection,
				    DBusMessage *message, int timeout_ms)
{
	DBusError error;
	dbus_error_init(&error);
	DBusMessage *reply = fplinux_bluetooth_call_cancellable(
		connection, message, timeout_ms, NULL, NULL, &error);
	if (reply == NULL) {
		fplinux_bluetooth_fail("D-Bus call failed: %s",
				       error.message ?: "unknown error");
		dbus_error_free(&error);
	}
	return reply;
}
