/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "bluez-fake.h"

#include <assert.h>
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ADAPTER_INTERFACE "org.bluez.Adapter1"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define ADAPTER_COUNT 4
#define REQUEST_COUNT 8

struct fake_adapter {
	char path[64];
	bool powered;
};

struct held_request {
	DBusMessage *message;
	char path[64];
	bool powered;
	enum bluez_fake_answer answer;
	uint64_t due_ms;
};

struct bluez_fake {
	DBusConnection *connection;
	struct fake_adapter adapters[ADAPTER_COUNT];
	struct held_request held[REQUEST_COUNT];
	enum bluez_fake_answer answer;
	unsigned int delay_ms;
	unsigned int requests;
};

static uint64_t now_ms(void)
{
	struct timespec now;

	assert(!clock_gettime(CLOCK_MONOTONIC, &now));
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static struct fake_adapter *find(struct bluez_fake *fake, const char *path)
{
	for (size_t i = 0; i < ADAPTER_COUNT; ++i)
		if (fake->adapters[i].path[0] &&
		    !strcmp(fake->adapters[i].path, path))
			return &fake->adapters[i];
	return NULL;
}

static void send_message(struct bluez_fake *fake, DBusMessage *message)
{
	assert(message);
	assert(dbus_connection_send(fake->connection, message, NULL));
	dbus_message_unref(message);
	dbus_connection_flush(fake->connection);
}

/* {"Powered": <powered>} as a{sv}. */
static void append_properties(DBusMessageIter *parent, bool powered)
{
	DBusMessageIter properties, entry, variant;
	const char *name = "Powered";
	dbus_bool_t value = powered;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}",
						&properties));
	assert(dbus_message_iter_open_container(
		&properties, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
	assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b",
						&variant));
	assert(dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN,
					      &value));
	assert(dbus_message_iter_close_container(&entry, &variant));
	assert(dbus_message_iter_close_container(&properties, &entry));
	assert(dbus_message_iter_close_container(parent, &properties));
}

/* {"org.bluez.Adapter1": {...}} as a{sa{sv}}. */
static void append_interfaces(DBusMessageIter *parent, bool powered)
{
	DBusMessageIter interfaces, entry;
	const char *name = ADAPTER_INTERFACE;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY,
						"{sa{sv}}", &interfaces));
	assert(dbus_message_iter_open_container(
		&interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
	append_properties(&entry, powered);
	assert(dbus_message_iter_close_container(&interfaces, &entry));
	assert(dbus_message_iter_close_container(parent, &interfaces));
}

static void reply_objects(struct bluez_fake *fake, DBusMessage *call)
{
	DBusMessage *reply = dbus_message_new_method_return(call);
	DBusMessageIter arguments, objects, entry;

	assert(reply);
	dbus_message_iter_init_append(reply, &arguments);
	assert(dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY,
						"{oa{sa{sv}}}", &objects));
	for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
		const char *path = fake->adapters[i].path;

		if (!path[0])
			continue;
		assert(dbus_message_iter_open_container(
			&objects, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
		assert(dbus_message_iter_append_basic(
			&entry, DBUS_TYPE_OBJECT_PATH, &path));
		append_interfaces(&entry, fake->adapters[i].powered);
		assert(dbus_message_iter_close_container(&objects, &entry));
	}
	assert(dbus_message_iter_close_container(&arguments, &objects));
	send_message(fake, reply);
}

static void signal_powered(struct bluez_fake *fake, const char *path,
			   bool powered)
{
	DBusMessage *signal =
		dbus_message_new_signal(path, PROPERTIES, "PropertiesChanged");
	DBusMessageIter arguments, invalidated;
	const char *interface = ADAPTER_INTERFACE;

	assert(signal);
	dbus_message_iter_init_append(signal, &arguments);
	assert(dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					      &interface));
	append_properties(&arguments, powered);
	assert(dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY,
						"s", &invalidated));
	assert(dbus_message_iter_close_container(&arguments, &invalidated));
	send_message(fake, signal);
}

static void hold_set(struct bluez_fake *fake, DBusMessage *call)
{
	DBusMessageIter arguments, variant;
	const char *interface, *property;
	dbus_bool_t powered;
	struct fake_adapter *adapter = find(fake, dbus_message_get_path(call));

	assert(adapter);
	assert(dbus_message_iter_init(call, &arguments));
	dbus_message_iter_get_basic(&arguments, &interface);
	assert(dbus_message_iter_next(&arguments));
	dbus_message_iter_get_basic(&arguments, &property);
	assert(dbus_message_iter_next(&arguments));
	assert(!strcmp(interface, ADAPTER_INTERFACE) &&
	       !strcmp(property, "Powered"));
	assert(dbus_message_iter_get_arg_type(&arguments) == DBUS_TYPE_VARIANT);
	dbus_message_iter_recurse(&arguments, &variant);
	assert(dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN);
	dbus_message_iter_get_basic(&variant, &powered);
	++fake->requests;
	for (size_t i = 0; i < REQUEST_COUNT; ++i) {
		struct held_request *held = &fake->held[i];

		if (held->message)
			continue;
		held->message = dbus_message_ref(call);
		snprintf(held->path, sizeof(held->path), "%s", adapter->path);
		held->powered = powered;
		held->answer = fake->answer;
		held->due_ms = now_ms() + fake->delay_ms;
		return;
	}
	assert(false);
}

static void answer_due(struct bluez_fake *fake)
{
	uint64_t now = now_ms();

	for (size_t i = 0; i < REQUEST_COUNT; ++i) {
		struct held_request *held = &fake->held[i];
		struct fake_adapter *adapter;

		if (!held->message || held->answer == BLUEZ_FAKE_SILENT ||
		    now < held->due_ms)
			continue;
		adapter = find(fake, held->path);
		if (held->answer == BLUEZ_FAKE_APPLY && adapter) {
			if (adapter->powered != held->powered) {
				adapter->powered = held->powered;
				signal_powered(fake, adapter->path,
					       held->powered);
			}
			send_message(fake, dbus_message_new_method_return(
						   held->message));
		} else {
			send_message(fake, dbus_message_new_error(
						   held->message,
						   "org.bluez.Error.Failed",
						   "Operation failed"));
		}
		dbus_message_unref(held->message);
		held->message = NULL;
	}
}

static DBusHandlerResult handle(DBusConnection *connection,
				DBusMessage *message, void *data)
{
	struct bluez_fake *fake = data;

	(void)connection;
	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (dbus_message_is_method_call(message, OBJECT_MANAGER,
					"GetManagedObjects") &&
	    !strcmp(dbus_message_get_path(message), "/"))
		reply_objects(fake, message);
	else if (dbus_message_is_method_call(message, PROPERTIES, "Set") &&
		 find(fake, dbus_message_get_path(message)))
		hold_set(fake, message);
	else
		send_message(fake, dbus_message_new_error(
					   message, DBUS_ERROR_UNKNOWN_METHOD,
					   "not modelled by the test"));
	return DBUS_HANDLER_RESULT_HANDLED;
}

struct bluez_fake *bluez_fake_start(void)
{
	struct bluez_fake *fake = calloc(1, sizeof(*fake));
	DBusError error;

	assert(fake);
	dbus_error_init(&error);
	fake->connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (!fake->connection)
		fprintf(stderr, "BlueZ fake: %s\n", error.message);
	assert(fake->connection);
	dbus_connection_set_exit_on_disconnect(fake->connection, FALSE);
	assert(dbus_bus_request_name(fake->connection, "org.bluez",
				     DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) ==
	       DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
	assert(dbus_connection_add_filter(fake->connection, handle, fake,
					  NULL));
	return fake;
}

void bluez_fake_stop(struct bluez_fake *fake)
{
	for (size_t i = 0; i < REQUEST_COUNT; ++i)
		if (fake->held[i].message)
			dbus_message_unref(fake->held[i].message);
	dbus_connection_close(fake->connection);
	dbus_connection_unref(fake->connection);
	free(fake);
}

void bluez_fake_pump(struct bluez_fake *fake)
{
	dbus_connection_read_write(fake->connection, 0);
	while (dbus_connection_dispatch(fake->connection) ==
	       DBUS_DISPATCH_DATA_REMAINS)
		;
	answer_due(fake);
}

static void signal_interfaces(struct bluez_fake *fake, const char *path,
			      bool added, bool powered)
{
	DBusMessage *signal = dbus_message_new_signal(
		"/", OBJECT_MANAGER,
		added ? "InterfacesAdded" : "InterfacesRemoved");
	DBusMessageIter arguments, names;
	const char *interface = ADAPTER_INTERFACE;

	assert(signal);
	dbus_message_iter_init_append(signal, &arguments);
	assert(dbus_message_iter_append_basic(&arguments, DBUS_TYPE_OBJECT_PATH,
					      &path));
	if (added) {
		append_interfaces(&arguments, powered);
	} else {
		assert(dbus_message_iter_open_container(
			&arguments, DBUS_TYPE_ARRAY, "s", &names));
		assert(dbus_message_iter_append_basic(&names, DBUS_TYPE_STRING,
						      &interface));
		assert(dbus_message_iter_close_container(&arguments, &names));
	}
	send_message(fake, signal);
}

void bluez_fake_add_adapter(struct bluez_fake *fake, const char *path,
			    bool powered)
{
	for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
		struct fake_adapter *adapter = &fake->adapters[i];

		if (adapter->path[0])
			continue;
		snprintf(adapter->path, sizeof(adapter->path), "%s", path);
		adapter->powered = powered;
		signal_interfaces(fake, path, true, powered);
		return;
	}
	assert(false);
}

void bluez_fake_remove_adapter(struct bluez_fake *fake, const char *path)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	adapter->path[0] = '\0';
	signal_interfaces(fake, path, false, false);
}

void bluez_fake_set_powered(struct bluez_fake *fake, const char *path,
			    bool powered)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	adapter->powered = powered;
	signal_powered(fake, path, powered);
}

bool bluez_fake_powered(const struct bluez_fake *fake, const char *path)
{
	for (size_t i = 0; i < ADAPTER_COUNT; ++i)
		if (!strcmp(fake->adapters[i].path, path))
			return fake->adapters[i].powered;
	assert(false);
	return false;
}

void bluez_fake_answer(struct bluez_fake *fake, enum bluez_fake_answer answer,
		       unsigned int delay_ms)
{
	fake->answer = answer;
	fake->delay_ms = delay_ms;
}

unsigned int bluez_fake_requests(const struct bluez_fake *fake)
{
	return fake->requests;
}
