// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-bluetooth-common.h"
#include "fplinux-bluetooth-pan.h"

#define PROPERTIES "org.freedesktop.DBus.Properties"

struct network_watch {
	const char *path;
	bool disconnected;
	bool owner_lost;
};

static bool get_property_bool(DBusConnection *connection, const char *path,
			      const char *interface, const char *property,
			      dbus_bool_t *output)
{
	DBusMessage *message = dbus_message_new_method_call("org.bluez", path,
							    PROPERTIES, "Get");
	DBusMessageIter iter, variant;
	const char *text = interface;
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &text,
				      DBUS_TYPE_STRING, &property,
				      DBUS_TYPE_INVALID)) {
		if (message != NULL)
			dbus_message_unref(message);
		return false;
	}
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply == NULL)
		return false;
	if (!dbus_message_iter_init(reply, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
		dbus_message_unref(reply);
		return false;
	}
	dbus_message_iter_recurse(&iter, &variant);
	if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) {
		dbus_message_unref(reply);
		return false;
	}
	dbus_message_iter_get_basic(&variant, output);
	dbus_message_unref(reply);
	return true;
}

static DBusHandlerResult network_filter(DBusConnection *connection,
					DBusMessage *message, void *data)
{
	(void)connection;
	struct network_watch *watch = data;
	if (dbus_message_is_signal(message, "org.freedesktop.DBus",
				   "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;
		if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING,
					  &name, DBUS_TYPE_STRING, &old_owner,
					  DBUS_TYPE_STRING, &new_owner,
					  DBUS_TYPE_INVALID) &&
		    strcmp(name, "org.bluez") == 0 && old_owner[0] != '\0' &&
		    new_owner[0] == '\0')
			watch->owner_lost = true;
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (!dbus_message_is_signal(message, PROPERTIES, "PropertiesChanged") ||
	    dbus_message_get_path(message) == NULL ||
	    strcmp(dbus_message_get_path(message), watch->path) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	DBusMessageIter iter, changed;
	const char *interface;
	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_get_basic(&iter, &interface);
	if (strcmp(interface, "org.bluez.Network1") != 0 ||
	    !dbus_message_iter_next(&iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_recurse(&iter, &changed);
	while (dbus_message_iter_get_arg_type(&changed) ==
	       DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *key;
		dbus_message_iter_recurse(&changed, &entry);
		if (dbus_message_iter_get_arg_type(&entry) ==
		    DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&entry, &key);
			if (dbus_message_iter_next(&entry) &&
			    dbus_message_iter_get_arg_type(&entry) ==
				    DBUS_TYPE_VARIANT) {
				dbus_bool_t connected;
				dbus_message_iter_recurse(&entry, &variant);
				if (strcmp(key, "Connected") == 0 &&
				    dbus_message_iter_get_arg_type(&variant) ==
					    DBUS_TYPE_BOOLEAN) {
					dbus_message_iter_get_basic(&variant,
								    &connected);
					if (!connected)
						watch->disconnected = true;
				}
			}
		}
		dbus_message_iter_next(&changed);
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int fplinux_bluetooth_pan_connect(const char *peer,
				  const volatile sig_atomic_t *interrupted)
{
	DBusConnection *connection = fplinux_bluetooth_system_bus();
	if (connection == NULL)
		return 1;
	char path[PATH_MAX];
	char device[18];
	for (size_t index = 0; index < sizeof(device) - 1; index++)
		device[index] =
			peer[index] == ':' ?
				'_' :
				(char)toupper((unsigned char)peer[index]);
	device[sizeof(device) - 1] = '\0';
	if (snprintf(path, sizeof(path), "/org/bluez/hci0/dev_%s", device) >=
	    (int)sizeof(path))
		return fplinux_bluetooth_fail("peer path is too long");
	struct network_watch watch = { .path = path };
	DBusError error;
	dbus_error_init(&error);
	char rule[PATH_MAX + 160];
	snprintf(
		rule, sizeof(rule),
		"type='signal',interface='%s',member='PropertiesChanged',path='%s'",
		PROPERTIES, path);
	dbus_bus_add_match(connection, rule, &error);
	dbus_bus_add_match(
		connection,
		"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
		&error);
	if (dbus_error_is_set(&error)) {
		fplinux_bluetooth_fail("cannot subscribe to PAN state: %s",
				       error.message);
		dbus_error_free(&error);
		return 1;
	}
	dbus_connection_flush(connection);
	if (!dbus_connection_add_filter(connection, network_filter, &watch,
					NULL))
		return fplinux_bluetooth_fail(
			"cannot install PAN state monitor");
	const char *role = "nap";
	DBusMessage *message = dbus_message_new_method_call(
		"org.bluez", path, "org.bluez.Network1", "Connect");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &role,
				      DBUS_TYPE_INVALID)) {
		dbus_connection_remove_filter(connection, network_filter,
					      &watch);
		return fplinux_bluetooth_fail(
			"cannot form Network1.Connect request");
	}
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 30000);
	if (reply == NULL) {
		dbus_connection_remove_filter(connection, network_filter,
					      &watch);
		return 1;
	}
	const char *interface;
	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &interface,
				   DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		dbus_connection_remove_filter(connection, network_filter,
					      &watch);
		return fplinux_bluetooth_fail(
			"Network1.Connect returned an invalid reply");
	}
	char interface_copy[IFNAMSIZ];
	if (snprintf(interface_copy, sizeof(interface_copy), "%s", interface) >=
	    (int)sizeof(interface_copy)) {
		dbus_message_unref(reply);
		dbus_connection_remove_filter(connection, network_filter,
					      &watch);
		return fplinux_bluetooth_fail(
			"Network1.Connect returned an overlong interface name");
	}
	dbus_message_unref(reply);
	dbus_bool_t connected;
	if (!get_property_bool(connection, path, "org.bluez.Network1",
			       "Connected", &connected)) {
		fplinux_bluetooth_fail("cannot read PAN Connected state");
		watch.owner_lost = true;
	} else if (!connected) {
		watch.disconnected = true;
	}
	if (watch.disconnected || watch.owner_lost) {
		dbus_connection_remove_filter(connection, network_filter,
					      &watch);
		message = dbus_message_new_method_call(
			"org.bluez", path, "org.bluez.Network1", "Disconnect");
		if (message != NULL) {
			reply = fplinux_bluetooth_call(connection, message,
						       15000);
			if (reply != NULL)
				dbus_message_unref(reply);
		}
		if (watch.owner_lost)
			return 1;
		return fplinux_bluetooth_fail(
			"PAN disconnected before its interface could be used");
	}
	if (printf("connected %s via %s; configure IP, DHCP and NAT separately\n",
		   peer, interface_copy) < 0 ||
	    fflush(stdout) == EOF) {
		fplinux_bluetooth_fail("cannot report BNEP interface: %s",
				       strerror(errno));
		watch.owner_lost = true;
	}
	while (!*interrupted && !watch.disconnected && !watch.owner_lost) {
		if (!dbus_connection_read_write_dispatch(connection, 1000)) {
			fplinux_bluetooth_fail(
				"system D-Bus disconnected while PAN was active");
			watch.owner_lost = true;
		}
	}
	dbus_connection_remove_filter(connection, network_filter, &watch);
	message = dbus_message_new_method_call(
		"org.bluez", path, "org.bluez.Network1", "Disconnect");
	if (message != NULL &&
	    dbus_message_append_args(message, DBUS_TYPE_INVALID)) {
		reply = fplinux_bluetooth_call(connection, message, 15000);
		if (reply != NULL)
			dbus_message_unref(reply);
	}
	return watch.owner_lost ? 1 : 0;
}
