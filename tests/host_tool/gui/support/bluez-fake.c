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
#define DEVICE_INTERFACE "org.bluez.Device1"
#define BATTERY_INTERFACE "org.bluez.Battery1"
#define AGENT_INTERFACE "org.bluez.Agent1"
#define AGENT_MANAGER "org.bluez.AgentManager1"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define ADAPTER_COUNT 4
#define REQUEST_COUNT 8
#define DEVICE_COUNT 8

struct fake_adapter {
	char path[64];
	bool powered;
	char alias[256];
	bool discoverable;
	uint32_t discoverable_timeout;
	bool pairable;
	uint32_t pairable_timeout;
	bool external_discovery;
	char discovery_owner[256];
};

struct fake_device {
	char path[256];
	char adapter[256];
	char address[18];
	char name[256];
	char alias[256];
	bool paired;
	bool trusted;
	bool connected;
	bool rssi_known;
	int16_t rssi;
	bool battery_known;
	uint8_t battery;
	size_t uuid_count;
	char uuids[32][37];
};

struct held_request {
	DBusMessage *message;
	enum bluez_fake_answer answer;
	uint64_t due_ms;
};

struct bluez_fake {
	DBusConnection *connection;
	struct fake_adapter adapters[ADAPTER_COUNT];
	struct fake_device devices[DEVICE_COUNT];
	struct held_request held[REQUEST_COUNT];
	enum bluez_fake_answer answer;
	unsigned int delay_ms;
	unsigned int requests;
	unsigned int discovery_requests;
	unsigned int cancel_requests;
	char agent_owner[256];
	char agent_path[256];
	DBusPendingCall *agent_call;
	DBusConnection *attacker;
	DBusPendingCall *forged_call;
	DBusMessage *pair_call;
	enum bluez_fake_agent_kind pairing_kind;
	uint32_t pairing_passkey;
	bool agent_answered;
	bool agent_rejected;
	char agent_code[17];
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

static struct fake_device *find_device(struct bluez_fake *fake,
				       const char *path)
{
	for (size_t i = 0; i < DEVICE_COUNT; ++i)
		if (fake->devices[i].path[0] &&
		    !strcmp(fake->devices[i].path, path))
			return &fake->devices[i];
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
static void append_value(DBusMessageIter *properties, const char *name,
			 int type, const void *value)
{
	DBusMessageIter entry, variant;
	char signature[] = { (char)type, '\0' };

	assert(dbus_message_iter_open_container(
		properties, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
	assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						signature, &variant));
	assert(dbus_message_iter_append_basic(&variant, type, value));
	assert(dbus_message_iter_close_container(&entry, &variant));
	assert(dbus_message_iter_close_container(properties, &entry));
}

static void append_properties(DBusMessageIter *parent,
			      const struct fake_adapter *adapter)
{
	DBusMessageIter properties;
	dbus_bool_t powered = adapter->powered;
	dbus_bool_t visible = adapter->discoverable;
	dbus_bool_t pairable = adapter->pairable;
	dbus_bool_t discovering = adapter->external_discovery ||
				  adapter->discovery_owner[0];
	const char *alias = adapter->alias;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}",
						&properties));
	append_value(&properties, "Powered", DBUS_TYPE_BOOLEAN, &powered);
	append_value(&properties, "Alias", DBUS_TYPE_STRING, &alias);
	append_value(&properties, "Discoverable", DBUS_TYPE_BOOLEAN, &visible);
	append_value(&properties, "DiscoverableTimeout", DBUS_TYPE_UINT32,
		     &adapter->discoverable_timeout);
	append_value(&properties, "Discovering", DBUS_TYPE_BOOLEAN,
		     &discovering);
	append_value(&properties, "Pairable", DBUS_TYPE_BOOLEAN, &pairable);
	append_value(&properties, "PairableTimeout", DBUS_TYPE_UINT32,
		     &adapter->pairable_timeout);
	assert(dbus_message_iter_close_container(parent, &properties));
}

/* {"org.bluez.Adapter1": {...}} as a{sa{sv}}. */
static void append_interfaces(DBusMessageIter *parent,
			      const struct fake_adapter *adapter)
{
	DBusMessageIter interfaces, entry;
	const char *name = ADAPTER_INTERFACE;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY,
						"{sa{sv}}", &interfaces));
	assert(dbus_message_iter_open_container(
		&interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
	append_properties(&entry, adapter);
	assert(dbus_message_iter_close_container(&interfaces, &entry));
	assert(dbus_message_iter_close_container(parent, &interfaces));
}

static void append_device_properties(DBusMessageIter *parent,
				     const struct fake_device *device)
{
	DBusMessageIter properties, entry, variant, uuids;
	const char *adapter = device->adapter, *address = device->address;
	const char *name = device->name, *alias = device->alias;
	const char *property = "UUIDs", *icon = "audio-card";
	dbus_bool_t paired = device->paired, trusted = device->trusted;
	dbus_bool_t connected = device->connected, resolved = device->connected;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}",
						&properties));
	append_value(&properties, "Adapter", DBUS_TYPE_OBJECT_PATH, &adapter);
	append_value(&properties, "Address", DBUS_TYPE_STRING, &address);
	append_value(&properties, "Name", DBUS_TYPE_STRING, &name);
	append_value(&properties, "Alias", DBUS_TYPE_STRING, &alias);
	append_value(&properties, "Icon", DBUS_TYPE_STRING, &icon);
	append_value(&properties, "Paired", DBUS_TYPE_BOOLEAN, &paired);
	append_value(&properties, "Trusted", DBUS_TYPE_BOOLEAN, &trusted);
	append_value(&properties, "Connected", DBUS_TYPE_BOOLEAN, &connected);
	append_value(&properties, "ServicesResolved", DBUS_TYPE_BOOLEAN,
		     &resolved);
	if (device->rssi_known)
		append_value(&properties, "RSSI", DBUS_TYPE_INT16,
			     &device->rssi);
	assert(dbus_message_iter_open_container(
		&properties, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
					      &property));
	assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as",
						&variant));
	assert(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s",
						&uuids));
	for (size_t i = 0; i < device->uuid_count; ++i) {
		const char *uuid = device->uuids[i];

		assert(dbus_message_iter_append_basic(&uuids, DBUS_TYPE_STRING,
						      &uuid));
	}
	assert(dbus_message_iter_close_container(&variant, &uuids));
	assert(dbus_message_iter_close_container(&entry, &variant));
	assert(dbus_message_iter_close_container(&properties, &entry));
	assert(dbus_message_iter_close_container(parent, &properties));
}

static void append_device_interfaces(DBusMessageIter *parent,
				     const struct fake_device *device)
{
	DBusMessageIter interfaces, entry, properties;
	const char *interface = DEVICE_INTERFACE;

	assert(dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY,
						"{sa{sv}}", &interfaces));
	assert(dbus_message_iter_open_container(
		&interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
					      &interface));
	append_device_properties(&entry, device);
	assert(dbus_message_iter_close_container(&interfaces, &entry));
	if (device->battery_known) {
		interface = BATTERY_INTERFACE;
		assert(dbus_message_iter_open_container(
			&interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
		assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						      &interface));
		assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
							"{sv}", &properties));
		append_value(&properties, "Percentage", DBUS_TYPE_BYTE,
			     &device->battery);
		assert(dbus_message_iter_close_container(&entry, &properties));
		assert(dbus_message_iter_close_container(&interfaces, &entry));
	}
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
	/* The map order must not decide whether a device belongs to an adapter. */
	for (size_t i = 0; i < DEVICE_COUNT; ++i) {
		const char *path = fake->devices[i].path;

		if (!path[0])
			continue;
		assert(dbus_message_iter_open_container(
			&objects, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
		assert(dbus_message_iter_append_basic(
			&entry, DBUS_TYPE_OBJECT_PATH, &path));
		append_device_interfaces(&entry, &fake->devices[i]);
		assert(dbus_message_iter_close_container(&objects, &entry));
	}
	for (size_t i = 0; i < ADAPTER_COUNT; ++i) {
		const char *path = fake->adapters[i].path;

		if (!path[0])
			continue;
		assert(dbus_message_iter_open_container(
			&objects, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
		assert(dbus_message_iter_append_basic(
			&entry, DBUS_TYPE_OBJECT_PATH, &path));
		append_interfaces(&entry, &fake->adapters[i]);
		assert(dbus_message_iter_close_container(&objects, &entry));
	}
	assert(dbus_message_iter_close_container(&arguments, &objects));
	send_message(fake, reply);
}

static void signal_property(struct bluez_fake *fake, const char *path,
			    const char *interface, const char *property,
			    int type, const void *value)
{
	DBusMessage *signal =
		dbus_message_new_signal(path, PROPERTIES, "PropertiesChanged");
	DBusMessageIter arguments, properties, invalidated;

	assert(signal);
	dbus_message_iter_init_append(signal, &arguments);
	assert(dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					      &interface));
	assert(dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY,
						"{sv}", &properties));
	if (value)
		append_value(&properties, property, type, value);
	assert(dbus_message_iter_close_container(&arguments, &properties));
	assert(dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY,
						"s", &invalidated));
	if (!value)
		assert(dbus_message_iter_append_basic(
			&invalidated, DBUS_TYPE_STRING, &property));
	assert(dbus_message_iter_close_container(&arguments, &invalidated));
	send_message(fake, signal);
}

static void signal_powered(struct bluez_fake *fake, const char *path,
			   bool powered)
{
	dbus_bool_t value = powered;

	signal_property(fake, path, ADAPTER_INTERFACE, "Powered",
			DBUS_TYPE_BOOLEAN, &value);
}

static void hold_set(struct bluez_fake *fake, DBusMessage *call)
{
	++fake->requests;
	for (size_t i = 0; i < REQUEST_COUNT; ++i) {
		struct held_request *held = &fake->held[i];

		if (held->message)
			continue;
		held->message = dbus_message_ref(call);
		held->answer = fake->answer;
		held->due_ms = now_ms() + fake->delay_ms;
		return;
	}
	assert(false);
}

static DBusMessage *apply_call(struct bluez_fake *fake, DBusMessage *call)
{
	const char *path = dbus_message_get_path(call);
	const char *member = dbus_message_get_member(call);
	struct fake_adapter *adapter = find(fake, path);
	struct fake_device *device = find_device(fake, path);

	if (dbus_message_is_method_call(call, PROPERTIES, "Set")) {
		DBusMessageIter arguments, value;
		const char *interface, *property, *text;
		dbus_bool_t boolean;
		dbus_uint32_t seconds;

		assert(dbus_message_iter_init(call, &arguments));
		dbus_message_iter_get_basic(&arguments, &interface);
		assert(dbus_message_iter_next(&arguments));
		dbus_message_iter_get_basic(&arguments, &property);
		assert(dbus_message_iter_next(&arguments));
		dbus_message_iter_recurse(&arguments, &value);
		if (!strcmp(property, "Alias")) {
			assert(dbus_message_iter_get_arg_type(&value) ==
			       DBUS_TYPE_STRING);
			dbus_message_iter_get_basic(&value, &text);
			if (adapter)
				snprintf(adapter->alias, sizeof(adapter->alias),
					 "%s", text[0] ? text : "Test adapter");
			else if (device)
				snprintf(device->alias, sizeof(device->alias),
					 "%s", text[0] ? text : device->name);
			else
				goto missing;
			signal_property(fake, path, interface, property,
					DBUS_TYPE_STRING, &text);
		} else if (adapter &&
			   (!strcmp(property, "DiscoverableTimeout") ||
			    !strcmp(property, "PairableTimeout"))) {
			assert(dbus_message_iter_get_arg_type(&value) ==
			       DBUS_TYPE_UINT32);
			dbus_message_iter_get_basic(&value, &seconds);
			if (!strcmp(property, "DiscoverableTimeout"))
				adapter->discoverable_timeout = seconds;
			else
				adapter->pairable_timeout = seconds;
			signal_property(fake, path, interface, property,
					DBUS_TYPE_UINT32, &seconds);
		} else {
			assert(dbus_message_iter_get_arg_type(&value) ==
			       DBUS_TYPE_BOOLEAN);
			dbus_message_iter_get_basic(&value, &boolean);
			if (adapter && !strcmp(property, "Powered")) {
				adapter->powered = boolean;
				if (!boolean)
					adapter->discovery_owner[0] = '\0';
			} else if (adapter && !strcmp(property, "Discoverable"))
				adapter->discoverable = boolean;
			else if (adapter && !strcmp(property, "Pairable"))
				adapter->pairable = boolean;
			else if (device && !strcmp(property, "Trusted"))
				device->trusted = boolean;
			else
				goto missing;
			signal_property(fake, path, interface, property,
					DBUS_TYPE_BOOLEAN, &boolean);
		}
	} else if (adapter && (!strcmp(member, "StartDiscovery") ||
			       !strcmp(member, "StopDiscovery"))) {
		dbus_bool_t discovering;

		++fake->discovery_requests;
		if (!strcmp(member, "StartDiscovery"))
			snprintf(adapter->discovery_owner,
				 sizeof(adapter->discovery_owner), "%s",
				 dbus_message_get_sender(call));
		else {
			if (strcmp(adapter->discovery_owner,
				   dbus_message_get_sender(call)))
				return dbus_message_new_error(
					call, "org.bluez.Error.NotAuthorized",
					"No session owned");
			adapter->discovery_owner[0] = '\0';
		}
		discovering = adapter->external_discovery ||
			      adapter->discovery_owner[0];
		signal_property(fake, path, ADAPTER_INTERFACE, "Discovering",
				DBUS_TYPE_BOOLEAN, &discovering);
	} else if (adapter && !strcmp(member, "RemoveDevice")) {
		const char *removed;

		assert(dbus_message_get_args(call, NULL, DBUS_TYPE_OBJECT_PATH,
					     &removed, DBUS_TYPE_INVALID));
		bluez_fake_remove_device(fake, removed);
	} else if (device && (!strcmp(member, "Connect") ||
			      !strcmp(member, "ConnectProfile") ||
			      !strcmp(member, "Disconnect"))) {
		bluez_fake_set_device_connected(fake, path,
						strcmp(member, "Disconnect"));
	} else if (device && !strcmp(member, "Pair")) {
		if (fake->pairing_kind != BLUEZ_FAKE_AGENT_NONE) {
			if (!fake->agent_owner[0])
				return dbus_message_new_error(
					call,
					"org.bluez.Error.AuthenticationFailed",
					"No agent");
			assert(!fake->pair_call);
			fake->pair_call = dbus_message_ref(call);
			bluez_fake_request_agent(fake, fake->pairing_kind, path,
						 fake->pairing_passkey, 0,
						 NULL);
			return NULL;
		}
		device->paired = true;
		{
			dbus_bool_t paired = TRUE;

			signal_property(fake, path, DEVICE_INTERFACE, "Paired",
					DBUS_TYPE_BOOLEAN, &paired);
		}
	} else if (device && !strcmp(member, "CancelPairing")) {
		if (fake->pair_call) {
			send_message(
				fake,
				dbus_message_new_error(
					fake->pair_call,
					"org.bluez.Error.AuthenticationCanceled",
					"Canceled"));
			dbus_message_unref(fake->pair_call);
			fake->pair_call = NULL;
		}
	} else {
		goto missing;
	}
	return dbus_message_new_method_return(call);
missing:
	return dbus_message_new_error(call, "org.bluez.Error.DoesNotExist",
				      "Object missing");
}

static void answer_due(struct bluez_fake *fake)
{
	uint64_t now = now_ms();

	for (size_t i = 0; i < REQUEST_COUNT; ++i) {
		struct held_request *held = &fake->held[i];
		DBusMessage *reply;

		if (!held->message || held->answer == BLUEZ_FAKE_SILENT ||
		    now < held->due_ms)
			continue;
		reply = held->answer == BLUEZ_FAKE_APPLY ?
				apply_call(fake, held->message) :
				dbus_message_new_error(held->message,
						       "org.bluez.Error.Failed",
						       "Operation failed");
		if (reply)
			send_message(fake, reply);
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
	else if (dbus_message_has_interface(message, AGENT_MANAGER)) {
		const char *path, *capability;

		if (dbus_message_has_member(message, "RegisterAgent")) {
			assert(dbus_message_get_args(
				message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_STRING, &capability,
				DBUS_TYPE_INVALID));
			assert(!strcmp(capability, "KeyboardDisplay"));
			snprintf(fake->agent_path, sizeof(fake->agent_path),
				 "%s", path);
			snprintf(fake->agent_owner, sizeof(fake->agent_owner),
				 "%s", dbus_message_get_sender(message));
		} else if (dbus_message_has_member(message,
						   "UnregisterAgent")) {
			fake->agent_owner[0] = '\0';
			fake->agent_path[0] = '\0';
		} else {
			assert(dbus_message_has_member(message,
						       "RequestDefaultAgent"));
			assert(!strcmp(fake->agent_owner,
				       dbus_message_get_sender(message)));
		}
		send_message(fake, dbus_message_new_method_return(message));
	} else if (dbus_message_is_method_call(message, PROPERTIES, "Set") &&
		   (find(fake, dbus_message_get_path(message)) ||
		    find_device(fake, dbus_message_get_path(message))))
		hold_set(fake, message);
	else if ((dbus_message_has_interface(message, ADAPTER_INTERFACE) &&
		  find(fake, dbus_message_get_path(message))) ||
		 (dbus_message_has_interface(message, DEVICE_INTERFACE) &&
		  find_device(fake, dbus_message_get_path(message)))) {
		if (dbus_message_has_member(message, "CancelPairing"))
			++fake->cancel_requests;
		hold_set(fake, message);
	} else
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
	assert(dbus_threads_init_default());
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
	if (fake->pair_call)
		dbus_message_unref(fake->pair_call);
	if (fake->agent_call) {
		dbus_pending_call_cancel(fake->agent_call);
		dbus_pending_call_unref(fake->agent_call);
	}
	if (fake->forged_call) {
		dbus_pending_call_cancel(fake->forged_call);
		dbus_pending_call_unref(fake->forged_call);
	}
	if (fake->attacker) {
		dbus_connection_close(fake->attacker);
		dbus_connection_unref(fake->attacker);
	}
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
	if (fake->attacker) {
		dbus_connection_read_write(fake->attacker, 0);
		while (dbus_connection_dispatch(fake->attacker) ==
		       DBUS_DISPATCH_DATA_REMAINS)
			;
	}
	answer_due(fake);
}

static void signal_interfaces(struct bluez_fake *fake, const char *path,
			      bool added)
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
		append_interfaces(&arguments, find(fake, path));
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
		snprintf(adapter->alias, sizeof(adapter->alias), "%s",
			 "Test adapter");
		adapter->discoverable_timeout = 180;
		signal_interfaces(fake, path, true);
		return;
	}
	assert(false);
}

void bluez_fake_remove_adapter(struct bluez_fake *fake, const char *path)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	adapter->path[0] = '\0';
	signal_interfaces(fake, path, false);
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

static void signal_device_interfaces(struct bluez_fake *fake,
				     const struct fake_device *device,
				     bool added)
{
	DBusMessage *message = dbus_message_new_signal(
		"/", OBJECT_MANAGER,
		added ? "InterfacesAdded" : "InterfacesRemoved");
	DBusMessageIter arguments, interfaces;
	const char *path = device->path, *interface = DEVICE_INTERFACE;

	assert(message);
	dbus_message_iter_init_append(message, &arguments);
	assert(dbus_message_iter_append_basic(&arguments, DBUS_TYPE_OBJECT_PATH,
					      &path));
	if (added)
		append_device_interfaces(&arguments, device);
	else {
		assert(dbus_message_iter_open_container(
			&arguments, DBUS_TYPE_ARRAY, "s", &interfaces));
		assert(dbus_message_iter_append_basic(
			&interfaces, DBUS_TYPE_STRING, &interface));
		assert(dbus_message_iter_close_container(&arguments,
							 &interfaces));
	}
	send_message(fake, message);
}

void bluez_fake_add_device(struct bluez_fake *fake, const char *path,
			   const char *address, const char *name, bool paired)
{
	for (size_t i = 0; i < DEVICE_COUNT; ++i) {
		struct fake_device *device = &fake->devices[i];
		char *slash;

		if (device->path[0])
			continue;
		*device = (struct fake_device){ 0 };
		snprintf(device->path, sizeof(device->path), "%s", path);
		snprintf(device->adapter, sizeof(device->adapter), "%s", path);
		slash = strrchr(device->adapter, '/');
		assert(slash);
		*slash = '\0';
		snprintf(device->address, sizeof(device->address), "%s",
			 address);
		snprintf(device->name, sizeof(device->name), "%s", name);
		snprintf(device->alias, sizeof(device->alias), "%s", name);
		device->paired = paired;
		signal_device_interfaces(fake, device, true);
		return;
	}
	assert(false);
}

void bluez_fake_remove_device(struct bluez_fake *fake, const char *path)
{
	struct fake_device *device = find_device(fake, path);

	assert(device);
	signal_device_interfaces(fake, device, false);
	memset(device, 0, sizeof(*device));
}

void bluez_fake_set_device_connected(struct bluez_fake *fake, const char *path,
				     bool connected)
{
	struct fake_device *device = find_device(fake, path);
	dbus_bool_t value = connected;

	assert(device);
	device->connected = connected;
	signal_property(fake, path, DEVICE_INTERFACE, "Connected",
			DBUS_TYPE_BOOLEAN, &value);
}

void bluez_fake_set_device_trusted(struct bluez_fake *fake, const char *path,
				   bool trusted)
{
	struct fake_device *device = find_device(fake, path);
	dbus_bool_t value = trusted;

	assert(device);
	device->trusted = trusted;
	signal_property(fake, path, DEVICE_INTERFACE, "Trusted",
			DBUS_TYPE_BOOLEAN, &value);
}

void bluez_fake_set_device_rssi(struct bluez_fake *fake, const char *path,
				int16_t rssi, bool known)
{
	struct fake_device *device = find_device(fake, path);

	assert(device);
	device->rssi = rssi;
	device->rssi_known = known;
	signal_property(fake, path, DEVICE_INTERFACE, "RSSI", DBUS_TYPE_INT16,
			known ? &rssi : NULL);
}

void bluez_fake_set_device_battery(struct bluez_fake *fake, const char *path,
				   uint8_t battery, bool known)
{
	struct fake_device *device = find_device(fake, path);

	assert(device);
	device->battery = battery;
	device->battery_known = known;
	if (known)
		signal_device_interfaces(fake, device, true);
	else
		signal_property(fake, path, BATTERY_INTERFACE, "Percentage",
				DBUS_TYPE_BYTE, NULL);
}

void bluez_fake_set_device_uuids(struct bluez_fake *fake, const char *path,
				 const char *const *uuids, size_t count)
{
	struct fake_device *device = find_device(fake, path);

	assert(device && count <= 32);
	device->uuid_count = count;
	for (size_t i = 0; i < count; ++i)
		snprintf(device->uuids[i], sizeof(device->uuids[i]), "%s",
			 uuids[i]);
	signal_device_interfaces(fake, device, true);
}

const char *bluez_fake_device_alias(struct bluez_fake *fake, const char *path)
{
	struct fake_device *device = find_device(fake, path);

	return device ? device->alias : NULL;
}

bool bluez_fake_device_paired(struct bluez_fake *fake, const char *path)
{
	struct fake_device *device = find_device(fake, path);

	return device && device->paired;
}

bool bluez_fake_device_trusted(struct bluez_fake *fake, const char *path)
{
	struct fake_device *device = find_device(fake, path);

	return device && device->trusted;
}

bool bluez_fake_device_connected(struct bluez_fake *fake, const char *path)
{
	struct fake_device *device = find_device(fake, path);

	return device && device->connected;
}

bool bluez_fake_discovering(struct bluez_fake *fake, const char *path)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	return adapter->external_discovery || adapter->discovery_owner[0];
}

void bluez_fake_set_external_discovery(struct bluez_fake *fake,
				       const char *path, bool discovering)
{
	struct fake_adapter *adapter = find(fake, path);
	dbus_bool_t value;

	assert(adapter);
	adapter->external_discovery = discovering;
	value = discovering || adapter->discovery_owner[0];
	signal_property(fake, path, ADAPTER_INTERFACE, "Discovering",
			DBUS_TYPE_BOOLEAN, &value);
}

unsigned int bluez_fake_discovery_requests(const struct bluez_fake *fake)
{
	return fake->discovery_requests;
}

unsigned int bluez_fake_cancel_requests(const struct bluez_fake *fake)
{
	return fake->cancel_requests;
}

void bluez_fake_set_pairable(struct bluez_fake *fake, const char *path,
			     bool pairable)
{
	struct fake_adapter *adapter = find(fake, path);
	dbus_bool_t value = pairable;

	assert(adapter);
	adapter->pairable = pairable;
	signal_property(fake, path, ADAPTER_INTERFACE, "Pairable",
			DBUS_TYPE_BOOLEAN, &value);
}

bool bluez_fake_pairable(struct bluez_fake *fake, const char *path)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	return adapter->pairable;
}

uint32_t bluez_fake_pairable_timeout(struct bluez_fake *fake, const char *path)
{
	struct fake_adapter *adapter = find(fake, path);

	assert(adapter);
	return adapter->pairable_timeout;
}

bool bluez_fake_agent_registered(const struct bluez_fake *fake)
{
	return fake->agent_owner[0];
}

void bluez_fake_set_pairing_request(struct bluez_fake *fake,
				    enum bluez_fake_agent_kind kind,
				    uint32_t passkey)
{
	fake->pairing_kind = kind;
	fake->pairing_passkey = passkey;
}

static void agent_answer(DBusPendingCall *call, void *data)
{
	struct bluez_fake *fake = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	const char *code;
	dbus_uint32_t passkey;

	if (call == fake->agent_call)
		fake->agent_call = NULL;
	else
		fake->forged_call = NULL;
	dbus_pending_call_unref(call);
	fake->agent_answered = true;
	fake->agent_rejected = !reply || dbus_message_get_type(reply) ==
						 DBUS_MESSAGE_TYPE_ERROR;
	fake->agent_code[0] = '\0';
	if (reply && dbus_message_has_signature(reply, "s") &&
	    dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &code,
				  DBUS_TYPE_INVALID))
		snprintf(fake->agent_code, sizeof(fake->agent_code), "%s",
			 code);
	else if (reply && dbus_message_has_signature(reply, "u") &&
		 dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &passkey,
				       DBUS_TYPE_INVALID))
		snprintf(fake->agent_code, sizeof(fake->agent_code), "%06u",
			 passkey);
	if (fake->pair_call) {
		struct fake_device *device = find_device(
			fake, dbus_message_get_path(fake->pair_call));

		if (!fake->agent_rejected && device) {
			dbus_bool_t paired = TRUE;

			device->paired = true;
			signal_property(fake, device->path, DEVICE_INTERFACE,
					"Paired", DBUS_TYPE_BOOLEAN, &paired);
			send_message(fake, dbus_message_new_method_return(
						   fake->pair_call));
		} else
			send_message(
				fake,
				dbus_message_new_error(
					fake->pair_call,
					"org.bluez.Error.AuthenticationRejected",
					"Pairing rejected"));
		dbus_message_unref(fake->pair_call);
		fake->pair_call = NULL;
	}
	if (reply)
		dbus_message_unref(reply);
}

static DBusMessage *new_agent_call(struct bluez_fake *fake,
				   enum bluez_fake_agent_kind kind,
				   const char *path, uint32_t passkey,
				   uint16_t entered, const char *text)
{
	static const char *const methods[] = {
		NULL,
		"RequestPinCode",
		"RequestPasskey",
		"RequestConfirmation",
		"RequestAuthorization",
		"AuthorizeService",
		"DisplayPinCode",
		"DisplayPasskey",
	};
	DBusMessage *message;

	assert(kind > BLUEZ_FAKE_AGENT_NONE &&
	       kind <= BLUEZ_FAKE_AGENT_DISPLAY_PASSKEY);
	assert(fake->agent_owner[0]);
	message = dbus_message_new_method_call(fake->agent_owner,
					       fake->agent_path,
					       AGENT_INTERFACE, methods[kind]);
	assert(message);
	assert(dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID));
	if (kind == BLUEZ_FAKE_AGENT_CONFIRMATION ||
	    kind == BLUEZ_FAKE_AGENT_DISPLAY_PASSKEY)
		assert(dbus_message_append_args(message, DBUS_TYPE_UINT32,
						&passkey, DBUS_TYPE_INVALID));
	if (kind == BLUEZ_FAKE_AGENT_DISPLAY_PASSKEY)
		assert(dbus_message_append_args(message, DBUS_TYPE_UINT16,
						&entered, DBUS_TYPE_INVALID));
	if (kind == BLUEZ_FAKE_AGENT_SERVICE ||
	    kind == BLUEZ_FAKE_AGENT_DISPLAY_PIN) {
		if (!text)
			text = kind == BLUEZ_FAKE_AGENT_SERVICE ?
				       "00001105-0000-1000-8000-00805f9b34fb" :
				       "004271";
		assert(dbus_message_append_args(message, DBUS_TYPE_STRING,
						&text, DBUS_TYPE_INVALID));
	}
	return message;
}

void bluez_fake_request_agent(struct bluez_fake *fake,
			      enum bluez_fake_agent_kind kind, const char *path,
			      uint32_t passkey, uint16_t entered,
			      const char *text)
{
	DBusMessage *message =
		new_agent_call(fake, kind, path, passkey, entered, text);

	assert(!fake->agent_call);
	fake->agent_answered = false;
	fake->agent_rejected = false;
	fake->agent_code[0] = '\0';
	assert(dbus_connection_send_with_reply(fake->connection, message,
					       &fake->agent_call, 3000));
	assert(fake->agent_call &&
	       dbus_pending_call_set_notify(fake->agent_call, agent_answer,
					    fake, NULL));
	dbus_message_unref(message);
	dbus_connection_flush(fake->connection);
}

bool bluez_fake_agent_answered(const struct bluez_fake *fake)
{
	return fake->agent_answered;
}

bool bluez_fake_agent_rejected(const struct bluez_fake *fake)
{
	return fake->agent_rejected;
}

const char *bluez_fake_agent_code(const struct bluez_fake *fake)
{
	return fake->agent_code;
}

void bluez_fake_forge_agent(struct bluez_fake *fake, const char *device)
{
	DBusError error;
	DBusMessage *message = new_agent_call(
		fake, BLUEZ_FAKE_AGENT_CONFIRMATION, device, 987654, 0, NULL);

	if (!fake->attacker) {
		dbus_error_init(&error);
		fake->attacker = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
		assert(fake->attacker);
		dbus_connection_set_exit_on_disconnect(fake->attacker, FALSE);
		dbus_error_free(&error);
	}
	assert(!fake->forged_call);
	fake->agent_answered = false;
	assert(dbus_connection_send_with_reply(fake->attacker, message,
					       &fake->forged_call, 3000));
	assert(fake->forged_call &&
	       dbus_pending_call_set_notify(fake->forged_call, agent_answer,
					    fake, NULL));
	dbus_message_unref(message);
	dbus_connection_flush(fake->attacker);
}

void bluez_fake_cancel_agent(struct bluez_fake *fake, bool release)
{
	DBusMessage *message = dbus_message_new_method_call(
		fake->agent_owner, fake->agent_path, AGENT_INTERFACE,
		release ? "Release" : "Cancel");

	assert(message);
	dbus_message_set_no_reply(message, TRUE);
	send_message(fake, message);
	if (release) {
		fake->agent_owner[0] = '\0';
		fake->agent_path[0] = '\0';
	}
}
