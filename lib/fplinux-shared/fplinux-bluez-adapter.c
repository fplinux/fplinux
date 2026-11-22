/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-bluez-adapter.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define BLUEZ_NAME "org.bluez"
#define BLUEZ_ADAPTER "org.bluez.Adapter1"
#define BLUEZ_DEVICE "org.bluez.Device1"
#define BLUEZ_BATTERY "org.bluez.Battery1"
#define BLUEZ_AGENT "org.bluez.Agent1"
#define BLUEZ_AGENT_MANAGER "org.bluez.AgentManager1"
#define AGENT_PATH "/org/fplinux/BluetoothAgent"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES "org.freedesktop.DBus.Properties"
/* The D-Bus specification's system bus when the environment names none. */
#define SYSTEM_BUS_ADDRESS "unix:path=/var/run/dbus/system_bus_socket"
#define REQUEST_TIMEOUT_MS 10000
/* libdbus watches one socket for reading and writing. */
#define WATCH_COUNT 4
#define TIMEOUT_COUNT 8
#define EVENT_COUNT 8
/* epoll data above 32 bits marks a timeout slot rather than a socket. */
#define TIMEOUT_TAG (UINT64_C(1) << 32)
#define PATH_BYTES FPLINUX_BLUEZ_PATH_BYTES

enum operation {
	OP_NONE,
	OP_POWER,
	OP_CHANGE,
	OP_VISIBILITY,
	OP_PAIRABLE_OFF,
	OP_DISCOVERY_START,
	OP_DISCOVERY_STOP,
	OP_PAIR,
};

struct inventory {
	unsigned int adapters;
	char path[PATH_BYTES];
	char address[18];
	char name[FPLINUX_BLUEZ_NAME_BYTES];
	bool powered;
	bool connectable;
	bool pairable;
	bool discoverable;
	bool discovering;
	uint32_t discoverable_timeout;
	size_t device_count;
	struct fplinux_bluez_device devices[FPLINUX_BLUEZ_DEVICE_COUNT];
};

struct timeout_slot {
	DBusTimeout *timeout;
	int fd;
};

struct fplinux_bluez_adapter {
	struct fplinux_bluez_adapter_listener listener;
	int fd;
	DBusConnection *connection;
	/* The bus closed the connection; it is released after dispatch. */
	bool lost;
	DBusWatch *watches[WATCH_COUNT];
	struct timeout_slot timeouts[TIMEOUT_COUNT];
	DBusPendingCall *hello;
	DBusPendingCall *owner_query;
	DBusPendingCall *snapshot;
	DBusPendingCall *mutation;
	DBusPendingCall *agent_call;
	char owner[256];
	struct inventory state;
	enum operation operation;
	bool requested;
	int timeout_ms;
	enum fplinux_bluez_visibility visibility;
	size_t policy_step;
	char operation_device[PATH_BYTES];
	int error;
	bool discovery_owned;
	bool agent_enabled;
	bool agent_registered;
	bool agent_ready;
	bool agent_default_pending;
	bool close_pairing;
	uint64_t agent_serial;
	struct fplinux_bluez_agent_request agent_request;
	DBusMessage *agent_message;
};

static void changed(struct fplinux_bluez_adapter *adapter)
{
	if (adapter->listener.changed)
		adapter->listener.changed(adapter->listener.context);
}

static void finished(struct fplinux_bluez_adapter *adapter, bool applied)
{
	if (adapter->listener.finished)
		adapter->listener.finished(applied, adapter->listener.context);
}

/* ---- Bus descriptors through the owner's single epoll descriptor ---- */

static bool update_socket(struct fplinux_bluez_adapter *adapter, int fd)
{
	struct epoll_event event = { .data.u64 = (uint64_t)fd };

	for (size_t i = 0; i < WATCH_COUNT; ++i) {
		DBusWatch *watch = adapter->watches[i];
		unsigned int flags;

		if (!watch || dbus_watch_get_unix_fd(watch) != fd ||
		    !dbus_watch_get_enabled(watch))
			continue;
		flags = dbus_watch_get_flags(watch);
		if (flags & DBUS_WATCH_READABLE)
			event.events |= EPOLLIN;
		if (flags & DBUS_WATCH_WRITABLE)
			event.events |= EPOLLOUT;
	}
	if (!event.events) {
		epoll_ctl(adapter->fd, EPOLL_CTL_DEL, fd, NULL);
		return true;
	}
	if (!epoll_ctl(adapter->fd, EPOLL_CTL_MOD, fd, &event))
		return true;
	return errno == ENOENT &&
	       !epoll_ctl(adapter->fd, EPOLL_CTL_ADD, fd, &event);
}

static dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	for (size_t i = 0; i < WATCH_COUNT; ++i) {
		if (adapter->watches[i])
			continue;
		adapter->watches[i] = watch;
		if (update_socket(adapter, dbus_watch_get_unix_fd(watch)))
			return TRUE;
		adapter->watches[i] = NULL;
		return FALSE;
	}
	return FALSE;
}

static void remove_watch(DBusWatch *watch, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	for (size_t i = 0; i < WATCH_COUNT; ++i)
		if (adapter->watches[i] == watch) {
			adapter->watches[i] = NULL;
			update_socket(adapter, dbus_watch_get_unix_fd(watch));
		}
}

static void toggle_watch(DBusWatch *watch, void *data)
{
	update_socket(data, dbus_watch_get_unix_fd(watch));
}

static bool arm_timeout(const struct timeout_slot *slot)
{
	struct itimerspec period = { 0 };

	if (dbus_timeout_get_enabled(slot->timeout)) {
		int interval = dbus_timeout_get_interval(slot->timeout);

		period.it_value.tv_sec = interval / 1000;
		period.it_value.tv_nsec = (long)(interval % 1000) * 1000000L;
		/* A zero value would disarm the timer instead. */
		if (!period.it_value.tv_sec && !period.it_value.tv_nsec)
			period.it_value.tv_nsec = 1;
		period.it_interval = period.it_value;
	}
	return !timerfd_settime(slot->fd, 0, &period, NULL);
}

static dbus_bool_t add_timeout(DBusTimeout *timeout, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	for (size_t i = 0; i < TIMEOUT_COUNT; ++i) {
		struct timeout_slot *slot = &adapter->timeouts[i];
		struct epoll_event event = {
			.events = EPOLLIN,
			.data.u64 = TIMEOUT_TAG | i,
		};

		if (slot->timeout)
			continue;
		slot->fd = timerfd_create(CLOCK_MONOTONIC,
					  TFD_NONBLOCK | TFD_CLOEXEC);
		if (slot->fd < 0)
			return FALSE;
		slot->timeout = timeout;
		if (!epoll_ctl(adapter->fd, EPOLL_CTL_ADD, slot->fd, &event) &&
		    arm_timeout(slot))
			return TRUE;
		close(slot->fd);
		*slot = (struct timeout_slot){ .fd = -1 };
		return FALSE;
	}
	return FALSE;
}

static void remove_timeout(DBusTimeout *timeout, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	for (size_t i = 0; i < TIMEOUT_COUNT; ++i) {
		struct timeout_slot *slot = &adapter->timeouts[i];

		if (slot->timeout != timeout)
			continue;
		/* Closing the timer also leaves the epoll set. */
		close(slot->fd);
		*slot = (struct timeout_slot){ .fd = -1 };
	}
}

static void toggle_timeout(DBusTimeout *timeout, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	for (size_t i = 0; i < TIMEOUT_COUNT; ++i)
		if (adapter->timeouts[i].timeout == timeout)
			arm_timeout(&adapter->timeouts[i]);
}

/* ---- Requests ---- */

static void cancel_call(DBusPendingCall **call)
{
	if (!*call)
		return;
	dbus_pending_call_cancel(*call);
	dbus_pending_call_unref(*call);
	*call = NULL;
}

/* Consumes message; *pending holds the call until its notify runs. */
static bool send_call(struct fplinux_bluez_adapter *adapter,
		      DBusMessage *message, int timeout_ms,
		      DBusPendingCallNotifyFunction notify,
		      DBusPendingCall **pending)
{
	DBusPendingCall *call = NULL;
	bool queued;

	if (!message)
		return false;
	/* An absent BlueZ is a normal state, not a reason to start it. */
	dbus_message_set_auto_start(message, FALSE);
	queued = dbus_connection_send_with_reply(adapter->connection, message,
						 &call, timeout_ms) &&
		 call;
	dbus_message_unref(message);
	if (!queued)
		return false;
	if (!dbus_pending_call_set_notify(call, notify, adapter, NULL)) {
		dbus_pending_call_cancel(call);
		dbus_pending_call_unref(call);
		return false;
	}
	*pending = call;
	return true;
}

/* The method return of a finished call, else NULL; consumes the call. */
static int reply_error(const char *name)
{
	if (!strcmp(name, DBUS_ERROR_NO_REPLY) ||
	    !strcmp(name, "org.bluez.Error.AuthenticationTimeout"))
		return ETIMEDOUT;
	if (!strcmp(name, "org.bluez.Error.NotReady"))
		return ENETDOWN;
	if (!strcmp(name, "org.bluez.Error.NotSupported") ||
	    !strcmp(name, "org.bluez.Error.BREDR.ProfileUnavailable"))
		return EOPNOTSUPP;
	if (!strcmp(name, "org.bluez.Error.InProgress"))
		return EBUSY;
	if (!strcmp(name, "org.bluez.Error.Canceled") ||
	    !strcmp(name, "org.bluez.Error.AuthenticationCanceled"))
		return ECANCELED;
	if (!strcmp(name, "org.bluez.Error.Rejected") ||
	    !strcmp(name, "org.bluez.Error.AuthenticationRejected") ||
	    !strcmp(name, "org.bluez.Error.AuthenticationFailed") ||
	    !strcmp(name, "org.bluez.Error.NotAuthorized") ||
	    !strcmp(name, DBUS_ERROR_ACCESS_DENIED))
		return EACCES;
	if (!strcmp(name, DBUS_ERROR_SERVICE_UNKNOWN) ||
	    !strcmp(name, DBUS_ERROR_NAME_HAS_NO_OWNER) ||
	    !strcmp(name, "org.bluez.Error.DoesNotExist"))
		return ENODEV;
	if (!strcmp(name, "org.bluez.Error.InvalidArguments") ||
	    !strcmp(name, DBUS_ERROR_INVALID_ARGS))
		return EINVAL;
	return EIO;
}

static DBusMessage *take_reply(DBusPendingCall **pending, int *error)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(*pending);

	dbus_pending_call_unref(*pending);
	*pending = NULL;
	if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		if (error)
			*error =
				reply_error(dbus_message_get_error_name(reply));
		dbus_message_unref(reply);
		return NULL;
	}
	if (error)
		*error = reply ? 0 : EIO;
	return reply;
}

static void send_reply(struct fplinux_bluez_adapter *adapter,
		       DBusMessage *reply)
{
	if (!reply)
		return;
	dbus_connection_send(adapter->connection, reply, NULL);
	dbus_message_unref(reply);
}

static void clear_agent_request(struct fplinux_bluez_adapter *adapter,
				const char *error)
{
	if (adapter->agent_message) {
		if (error && adapter->connection)
			send_reply(adapter,
				   dbus_message_new_error(
					   adapter->agent_message, error,
					   "Pairing request ended"));
		dbus_message_unref(adapter->agent_message);
		adapter->agent_message = NULL;
	}
	adapter->agent_request = (struct fplinux_bluez_agent_request){ 0 };
}

static void close_pairing_if_idle(struct fplinux_bluez_adapter *adapter);
static void register_agent(struct fplinux_bluez_adapter *adapter);

static void end_operation(struct fplinux_bluez_adapter *adapter, int error)
{
	bool automatic = adapter->operation == OP_PAIRABLE_OFF;

	adapter->operation = OP_NONE;
	adapter->operation_device[0] = '\0';
	adapter->error = error;
	if (automatic) {
		register_agent(adapter);
		changed(adapter);
	} else {
		finished(adapter, !error);
	}
	close_pairing_if_idle(adapter);
}

static void replace_inventory(struct fplinux_bluez_adapter *adapter,
			      const struct inventory *state)
{
	bool moved = strcmp(adapter->state.path, state->path);
	bool different = memcmp(&adapter->state, state, sizeof(*state));

	adapter->state = *state;
	if (!state->powered || !state->discovering)
		adapter->discovery_owned = false;
	if (state->adapters != 1 || moved) {
		adapter->discovery_owned = false;
		clear_agent_request(adapter, "org.bluez.Error.Canceled");
	}
	if (adapter->mutation &&
	    (state->adapters != 1 || moved ||
	     (adapter->operation_device[0] &&
	      !fplinux_bluez_adapter_device(adapter,
					    adapter->operation_device)))) {
		cancel_call(&adapter->mutation);
		end_operation(adapter, ENODEV);
	}
	if (adapter->agent_request.device[0] &&
	    !fplinux_bluez_adapter_device(adapter,
					  adapter->agent_request.device))
		clear_agent_request(adapter, "org.bluez.Error.Canceled");
	if (different)
		changed(adapter);
}

static bool read_basic(DBusMessageIter *value, int type, void *result)
{
	if (dbus_message_iter_get_arg_type(value) != type)
		return false;
	dbus_message_iter_get_basic(value, result);
	return true;
}

static void read_text(DBusMessageIter *value, char *result, size_t bytes)
{
	const char *text;

	if (read_basic(value, DBUS_TYPE_STRING, &text))
		snprintf(result, bytes, "%s", text);
}

static void read_properties(DBusMessageIter *properties,
			    struct inventory *state,
			    struct fplinux_bluez_device *device,
			    const char *interface, char *device_adapter)
{
	DBusMessageIter entry;

	if (dbus_message_iter_get_arg_type(properties) != DBUS_TYPE_ARRAY)
		return;
	dbus_message_iter_recurse(properties, &entry);
	for (; dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(&entry)) {
		DBusMessageIter field, value;
		const char *name;
		dbus_bool_t boolean;

		dbus_message_iter_recurse(&entry, &field);
		if (dbus_message_iter_get_arg_type(&field) != DBUS_TYPE_STRING)
			continue;
		dbus_message_iter_get_basic(&field, &name);
		if (!dbus_message_iter_next(&field) ||
		    dbus_message_iter_get_arg_type(&field) != DBUS_TYPE_VARIANT)
			continue;
		dbus_message_iter_recurse(&field, &value);
		if (!strcmp(interface, BLUEZ_ADAPTER)) {
			if (!strcmp(name, "Alias"))
				read_text(&value, state->name,
					  sizeof(state->name));
			else if (!strcmp(name, "Address"))
				read_text(&value, state->address,
					  sizeof(state->address));
			else if (!strcmp(name, "Powered") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				state->powered = boolean;
			else if (!strcmp(name, "Connectable") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				state->connectable = boolean;
			else if (!strcmp(name, "Pairable") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				state->pairable = boolean;
			else if (!strcmp(name, "Discoverable") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				state->discoverable = boolean;
			else if (!strcmp(name, "Discovering") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				state->discovering = boolean;
			else if (!strcmp(name, "DiscoverableTimeout"))
				read_basic(&value, DBUS_TYPE_UINT32,
					   &state->discoverable_timeout);
		} else if (device && !strcmp(interface, BLUEZ_BATTERY)) {
			if (!strcmp(name, "Percentage") &&
			    read_basic(&value, DBUS_TYPE_BYTE,
				       &device->battery))
				device->battery_known = device->battery <= 100;
		} else if (device && !strcmp(interface, BLUEZ_DEVICE)) {
			if (!strcmp(name, "Adapter") && device_adapter) {
				const char *path;

				if (read_basic(&value, DBUS_TYPE_OBJECT_PATH,
					       &path))
					snprintf(device_adapter, PATH_BYTES,
						 "%s", path);
			} else if (!strcmp(name, "Address"))
				read_text(&value, device->address,
					  sizeof(device->address));
			else if (!strcmp(name, "Name"))
				read_text(&value, device->name,
					  sizeof(device->name));
			else if (!strcmp(name, "Alias"))
				read_text(&value, device->alias,
					  sizeof(device->alias));
			else if (!strcmp(name, "Icon"))
				read_text(&value, device->icon,
					  sizeof(device->icon));
			else if (!strcmp(name, "Paired") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				device->paired = boolean;
			else if (!strcmp(name, "Trusted") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				device->trusted = boolean;
			else if (!strcmp(name, "Connected") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				device->connected = boolean;
			else if (!strcmp(name, "ServicesResolved") &&
				 read_basic(&value, DBUS_TYPE_BOOLEAN,
					    &boolean))
				device->services_resolved = boolean;
			else if (!strcmp(name, "RSSI") &&
				 read_basic(&value, DBUS_TYPE_INT16,
					    &device->rssi))
				device->rssi_known = true;
			else if (!strcmp(name, "UUIDs") &&
				 dbus_message_iter_get_arg_type(&value) ==
					 DBUS_TYPE_ARRAY) {
				DBusMessageIter uuid;

				device->uuid_count = 0;
				dbus_message_iter_recurse(&value, &uuid);
				for (; dbus_message_iter_get_arg_type(&uuid) ==
				       DBUS_TYPE_STRING;
				     dbus_message_iter_next(&uuid)) {
					if (device->uuid_count ==
					    FPLINUX_BLUEZ_UUID_COUNT)
						break;
					read_text(
						&uuid,
						device->uuids
							[device->uuid_count++],
						FPLINUX_BLUEZ_UUID_BYTES);
				}
			}
		}
	}
}

/* Counts the Adapter1 objects of an a{oa{sa{sv}}} snapshot. */
static void read_snapshot(struct fplinux_bluez_adapter *adapter,
			  DBusMessage *reply)
{
	DBusMessageIter objects, object;
	struct inventory *state = calloc(1, sizeof(*state));

	if (!state)
		return;
	if (!reply || !dbus_message_has_signature(reply, "a{oa{sa{sv}}}") ||
	    !dbus_message_iter_init(reply, &objects)) {
		replace_inventory(adapter, state);
		free(state);
		return;
	}
	/* Device1 and Battery1 may precede the adapter in the object map. */
	for (int pass = 0; pass < 2; ++pass) {
		dbus_message_iter_recurse(&objects, &object);
		for (; dbus_message_iter_get_arg_type(&object) ==
		       DBUS_TYPE_DICT_ENTRY;
		     dbus_message_iter_next(&object)) {
			DBusMessageIter field, interfaces;
			const char *path;
			struct fplinux_bluez_device device = { 0 };
			char device_adapter[PATH_BYTES] = "";
			bool is_device = false;

			dbus_message_iter_recurse(&object, &field);
			if (dbus_message_iter_get_arg_type(&field) !=
			    DBUS_TYPE_OBJECT_PATH)
				continue;
			dbus_message_iter_get_basic(&field, &path);
			if (!dbus_message_iter_next(&field) ||
			    dbus_message_iter_get_arg_type(&field) !=
				    DBUS_TYPE_ARRAY)
				continue;
			dbus_message_iter_recurse(&field, &interfaces);
			for (; dbus_message_iter_get_arg_type(&interfaces) ==
			       DBUS_TYPE_DICT_ENTRY;
			     dbus_message_iter_next(&interfaces)) {
				DBusMessageIter interface;
				const char *name;

				dbus_message_iter_recurse(&interfaces,
							  &interface);
				if (dbus_message_iter_get_arg_type(
					    &interface) != DBUS_TYPE_STRING)
					continue;
				dbus_message_iter_get_basic(&interface, &name);
				if (!pass && !strcmp(name, BLUEZ_ADAPTER)) {
					if (state->adapters++)
						continue;
					snprintf(state->path,
						 sizeof(state->path), "%s",
						 path);
					dbus_message_iter_next(&interface);
					read_properties(&interface, state, NULL,
							name, NULL);
				} else if (pass &&
					   (!strcmp(name, BLUEZ_DEVICE) ||
					    !strcmp(name, BLUEZ_BATTERY))) {
					is_device |=
						!strcmp(name, BLUEZ_DEVICE);
					dbus_message_iter_next(&interface);
					read_properties(&interface, state,
							&device, name,
							device_adapter);
				}
			}
			if (pass && is_device && state->adapters == 1 &&
			    !strcmp(device_adapter, state->path) &&
			    strlen(path) < sizeof(device.path) &&
			    state->device_count < FPLINUX_BLUEZ_DEVICE_COUNT) {
				snprintf(device.path, sizeof(device.path), "%s",
					 path);
				state->devices[state->device_count++] = device;
			}
		}
	}
	if (state->adapters != 1) {
		unsigned int count = state->adapters;

		memset(state, 0, sizeof(*state));
		state->adapters = count;
	}
	replace_inventory(adapter, state);
	free(state);
}

static void snapshot_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;

	(void)call;
	reply = take_reply(&adapter->snapshot, NULL);
	read_snapshot(adapter, reply);
	if (reply)
		dbus_message_unref(reply);
}

/* A newer snapshot replaces one still on its way. */
static void query_adapters(struct fplinux_bluez_adapter *adapter)
{
	cancel_call(&adapter->snapshot);
	if (!adapter->owner[0])
		return;
	if (!send_call(adapter,
		       dbus_message_new_method_call(adapter->owner, "/",
						    OBJECT_MANAGER,
						    "GetManagedObjects"),
		       REQUEST_TIMEOUT_MS, snapshot_done, &adapter->snapshot))
		adapter->error = ENOMEM;
}

static DBusMessage *property_message(struct fplinux_bluez_adapter *adapter,
				     const char *path, const char *interface,
				     const char *property, int type,
				     const void *value)
{
	DBusMessage *message = dbus_message_new_method_call(
		adapter->owner, path, PROPERTIES, "Set");
	DBusMessageIter arguments, variant;
	char signature[] = { (char)type, '\0' };

	if (!message)
		return NULL;
	dbus_message_iter_init_append(message, &arguments);
	if (!dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					    &interface) ||
	    !dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					    &property) ||
	    !dbus_message_iter_open_container(&arguments, DBUS_TYPE_VARIANT,
					      signature, &variant)) {
		dbus_message_unref(message);
		return NULL;
	}
	if (!dbus_message_iter_append_basic(&variant, type, value) ||
	    !dbus_message_iter_close_container(&arguments, &variant)) {
		dbus_message_iter_abandon_container(&arguments, &variant);
		dbus_message_unref(message);
		return NULL;
	}
	return message;
}

static void mutation_done(DBusPendingCall *call, void *data);

static void close_pairing_if_idle(struct fplinux_bluez_adapter *adapter)
{
	dbus_bool_t enabled = FALSE;

	if (!adapter->close_pairing || adapter->mutation)
		return;
	adapter->close_pairing = false;
	if (!fplinux_bluez_adapter_powered(adapter))
		return;
	if (send_call(adapter,
		      property_message(adapter, adapter->state.path,
				       BLUEZ_ADAPTER, "Pairable",
				       DBUS_TYPE_BOOLEAN, &enabled),
		      REQUEST_TIMEOUT_MS, mutation_done, &adapter->mutation))
		adapter->operation = OP_PAIRABLE_OFF;
}

struct mode_change {
	const char *name;
	bool enabled;
	bool timeout;
};

static const struct mode_change power_on[] = {
	{ .name = "Connectable", .enabled = true },
};

static const struct mode_change show_adapter[] = {
	{ .name = "PairableTimeout", .timeout = true },
	{ .name = "Connectable", .enabled = true },
	{ .name = "Pairable", .enabled = true },
	{ .name = "DiscoverableTimeout", .timeout = true },
	{ .name = "Discoverable", .enabled = true },
};

static const struct mode_change hide_adapter[] = {
	{ .name = "Discoverable" },
	/* BlueZ can turn off page scan when it disables discoverability. */
	{ .name = "Connectable", .enabled = true },
	{ .name = "Pairable" },
};

static DBusMessage *mode_message(struct fplinux_bluez_adapter *adapter,
				 const struct mode_change *change)
{
	dbus_bool_t enabled = change->enabled;
	dbus_uint32_t seconds =
		adapter->visibility == FPLINUX_BLUEZ_VISIBLE_TIMED ? 120 : 0;

	return property_message(
		adapter, adapter->state.path, BLUEZ_ADAPTER, change->name,
		change->timeout ? DBUS_TYPE_UINT32 : DBUS_TYPE_BOOLEAN,
		change->timeout ? (const void *)&seconds :
				  (const void *)&enabled);
}

static const struct mode_change *
next_mode_change(struct fplinux_bluez_adapter *adapter)
{
	const struct mode_change *steps;
	size_t count;

	if (adapter->operation == OP_POWER && adapter->requested) {
		steps = power_on;
		count = sizeof(power_on) / sizeof(power_on[0]);
	} else if (adapter->operation != OP_VISIBILITY) {
		return NULL;
	} else if (adapter->visibility == FPLINUX_BLUEZ_HIDDEN) {
		steps = hide_adapter;
		count = sizeof(hide_adapter) / sizeof(hide_adapter[0]);
	} else {
		steps = show_adapter;
		count = sizeof(show_adapter) / sizeof(show_adapter[0]);
	}
	return adapter->policy_step < count ? &steps[adapter->policy_step++] :
					      NULL;
}

static void connectable_done(DBusPendingCall *call, void *data);

static bool query_connectable(struct fplinux_bluez_adapter *adapter)
{
	const char *interface = BLUEZ_ADAPTER, *property = "Connectable";
	DBusMessage *message = dbus_message_new_method_call(
		adapter->owner, adapter->state.path, PROPERTIES, "Get");

	if (message &&
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &interface,
				      DBUS_TYPE_STRING, &property,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		return false;
	}
	return send_call(adapter, message, adapter->timeout_ms,
			 connectable_done, &adapter->mutation);
}

static void continue_mutation(struct fplinux_bluez_adapter *adapter, int error)
{
	bool power_changed = false;

	if (!error) {
		const struct mode_change *change = next_mode_change(adapter);

		if (change) {
			bool queued;

			/* Read after the previous Set, before property signals
			 * necessarily reach this client. A redundant Set can
			 * get stuck behind a completed controller request.
			 */
			if (!strcmp(change->name, "Connectable"))
				queued = query_connectable(adapter);
			else
				queued = send_call(
					adapter, mode_message(adapter, change),
					adapter->timeout_ms, mutation_done,
					&adapter->mutation);
			if (queued)
				return;
			error = ENOMEM;
		}
		if (!error && adapter->operation == OP_POWER &&
		    adapter->state.powered != adapter->requested) {
			adapter->state.powered = adapter->requested;
			power_changed = true;
		}
		if (adapter->operation == OP_DISCOVERY_START)
			adapter->discovery_owned = true;
		else if (adapter->operation == OP_DISCOVERY_STOP ||
			 (adapter->operation == OP_POWER &&
			  !adapter->requested))
			adapter->discovery_owned = false;
	}
	query_adapters(adapter);
	if (adapter->operation == OP_PAIR)
		clear_agent_request(adapter, "org.bluez.Error.Canceled");
	end_operation(adapter, error);
	if (power_changed)
		changed(adapter);
}

static void mutation_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;
	int error;

	(void)call;
	reply = take_reply(&adapter->mutation, &error);
	if (reply)
		dbus_message_unref(reply);
	continue_mutation(adapter, error);
}

static void connectable_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessageIter arguments, value;
	dbus_bool_t connectable = FALSE;
	DBusMessage *reply;
	int error;

	(void)call;
	reply = take_reply(&adapter->mutation, &error);
	if (!reply) {
		continue_mutation(adapter, error);
		return;
	}
	if (!dbus_message_has_signature(reply, "v") ||
	    !dbus_message_iter_init(reply, &arguments)) {
		error = EPROTO;
	} else {
		dbus_message_iter_recurse(&arguments, &value);
		if (!read_basic(&value, DBUS_TYPE_BOOLEAN, &connectable))
			error = EPROTO;
	}
	dbus_message_unref(reply);
	if (!error && !connectable) {
		const dbus_bool_t enabled = TRUE;

		if (send_call(adapter,
			      property_message(adapter, adapter->state.path,
					       BLUEZ_ADAPTER, "Connectable",
					       DBUS_TYPE_BOOLEAN, &enabled),
			      adapter->timeout_ms, mutation_done,
			      &adapter->mutation))
			return;
		error = ENOMEM;
	}
	continue_mutation(adapter, error);
}

static void hello_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;
	const char *name;

	(void)call;
	reply = take_reply(&adapter->hello, NULL);
	if (reply && dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_INVALID))
		dbus_bus_set_unique_name(adapter->connection, name);
	else
		adapter->lost = true;
	if (reply)
		dbus_message_unref(reply);
}

static void agent_done(DBusPendingCall *call, void *data);

static DBusMessage *agent_manager_message(struct fplinux_bluez_adapter *adapter,
					  const char *method)
{
	DBusMessage *message = dbus_message_new_method_call(
		adapter->owner, "/org/bluez", BLUEZ_AGENT_MANAGER, method);
	const char *path = AGENT_PATH;
	const char *capability = "KeyboardDisplay";
	bool valid = message &&
		     dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH,
					      &path, DBUS_TYPE_INVALID);

	if (valid && !strcmp(method, "RegisterAgent"))
		valid = dbus_message_append_args(message, DBUS_TYPE_STRING,
						 &capability,
						 DBUS_TYPE_INVALID);
	if (!valid && message) {
		dbus_message_unref(message);
		message = NULL;
	}
	return message;
}

static void register_agent(struct fplinux_bluez_adapter *adapter)
{
	if (!adapter->agent_enabled || !adapter->owner[0] ||
	    adapter->agent_registered || adapter->agent_call ||
	    adapter->operation == OP_PAIRABLE_OFF)
		return;
	if (!send_call(adapter, agent_manager_message(adapter, "RegisterAgent"),
		       REQUEST_TIMEOUT_MS, agent_done, &adapter->agent_call)) {
		adapter->error = ENOMEM;
		changed(adapter);
	}
}

static void agent_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;
	int error;

	(void)call;
	reply = take_reply(&adapter->agent_call, &error);
	if (reply)
		dbus_message_unref(reply);
	if (!error && !adapter->agent_default_pending) {
		adapter->agent_registered = true;
		adapter->agent_default_pending = true;
		if (send_call(adapter,
			      agent_manager_message(adapter,
						    "RequestDefaultAgent"),
			      REQUEST_TIMEOUT_MS, agent_done,
			      &adapter->agent_call))
			return;
		error = ENOMEM;
	} else if (!error) {
		adapter->agent_ready = true;
	}
	adapter->agent_default_pending = false;
	adapter->error = error;
	changed(adapter);
}

static void abort_pair(struct fplinux_bluez_adapter *adapter)
{
	clear_agent_request(adapter, "org.bluez.Error.Canceled");
	if (adapter->operation == OP_PAIR) {
		cancel_call(&adapter->mutation);
		end_operation(adapter, ECANCELED);
	}
}

static DBusHandlerResult agent_message(DBusConnection *connection,
				       DBusMessage *message, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	struct fplinux_bluez_agent_request request = { 0 };
	const char *device = NULL, *text = NULL;
	bool display = false, valid = false;

	(void)connection;
	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL ||
	    !dbus_message_has_interface(message, BLUEZ_AGENT))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!adapter->owner[0] ||
	    !dbus_message_has_sender(message, adapter->owner)) {
		send_reply(
			adapter,
			dbus_message_new_error(
				message, DBUS_ERROR_ACCESS_DENIED,
				"Only Bluetooth service may call the agent"));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(message, BLUEZ_AGENT, "Cancel") ||
	    dbus_message_is_method_call(message, BLUEZ_AGENT, "Release")) {
		if (!dbus_message_has_signature(message, ""))
			goto invalid;
		abort_pair(adapter);
		if (dbus_message_has_member(message, "Release")) {
			cancel_call(&adapter->agent_call);
			adapter->agent_registered = false;
			adapter->agent_ready = false;
			adapter->agent_default_pending = false;
		}
		send_reply(adapter, dbus_message_new_method_return(message));
		changed(adapter);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!adapter->agent_enabled || !adapter->agent_registered ||
	    !fplinux_bluez_adapter_present(adapter))
		goto rejected;
	if (dbus_message_has_member(message, "RequestPinCode")) {
		request.kind = FPLINUX_BLUEZ_AGENT_PIN;
		valid = dbus_message_get_args(message, NULL,
					      DBUS_TYPE_OBJECT_PATH, &device,
					      DBUS_TYPE_INVALID);
	} else if (dbus_message_has_member(message, "RequestPasskey")) {
		request.kind = FPLINUX_BLUEZ_AGENT_PASSKEY;
		valid = dbus_message_get_args(message, NULL,
					      DBUS_TYPE_OBJECT_PATH, &device,
					      DBUS_TYPE_INVALID);
	} else if (dbus_message_has_member(message, "RequestConfirmation")) {
		request.kind = FPLINUX_BLUEZ_AGENT_CONFIRMATION;
		valid = dbus_message_get_args(
			message, NULL, DBUS_TYPE_OBJECT_PATH, &device,
			DBUS_TYPE_UINT32, &request.passkey, DBUS_TYPE_INVALID);
	} else if (dbus_message_has_member(message, "RequestAuthorization")) {
		request.kind = FPLINUX_BLUEZ_AGENT_AUTHORIZATION;
		valid = dbus_message_get_args(message, NULL,
					      DBUS_TYPE_OBJECT_PATH, &device,
					      DBUS_TYPE_INVALID);
	} else if (dbus_message_has_member(message, "AuthorizeService")) {
		request.kind = FPLINUX_BLUEZ_AGENT_SERVICE;
		valid = dbus_message_get_args(message, NULL,
					      DBUS_TYPE_OBJECT_PATH, &device,
					      DBUS_TYPE_STRING, &text,
					      DBUS_TYPE_INVALID);
		if (valid)
			snprintf(request.uuid, sizeof(request.uuid), "%s",
				 text);
	} else if (dbus_message_has_member(message, "DisplayPinCode")) {
		display = true;
		request.kind = FPLINUX_BLUEZ_AGENT_DISPLAY_PIN;
		valid = dbus_message_get_args(message, NULL,
					      DBUS_TYPE_OBJECT_PATH, &device,
					      DBUS_TYPE_STRING, &text,
					      DBUS_TYPE_INVALID);
		if (valid && strlen(text) <= 16)
			snprintf(request.pin, sizeof(request.pin), "%s", text);
		else
			valid = false;
	} else if (dbus_message_has_member(message, "DisplayPasskey")) {
		display = true;
		request.kind = FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY;
		valid = dbus_message_get_args(
			message, NULL, DBUS_TYPE_OBJECT_PATH, &device,
			DBUS_TYPE_UINT32, &request.passkey, DBUS_TYPE_UINT16,
			&request.entered, DBUS_TYPE_INVALID);
	} else {
		send_reply(adapter, dbus_message_new_error(
					    message, DBUS_ERROR_UNKNOWN_METHOD,
					    "Unknown agent method"));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!valid || request.passkey > 999999 || request.entered > 6)
		goto invalid;
	/* An incoming request can follow InterfacesAdded before its query returns. */
	if (strlen(device) >= sizeof(request.device) ||
	    strncmp(device, adapter->state.path, strlen(adapter->state.path)) ||
	    device[strlen(adapter->state.path)] != '/')
		goto rejected;
	clear_agent_request(adapter, "org.bluez.Error.Canceled");
	request.serial = ++adapter->agent_serial;
	if (!request.serial)
		request.serial = ++adapter->agent_serial;
	snprintf(request.device, sizeof(request.device), "%s", device);
	adapter->agent_request = request;
	if (display)
		send_reply(adapter, dbus_message_new_method_return(message));
	else
		adapter->agent_message = dbus_message_ref(message);
	changed(adapter);
	return DBUS_HANDLER_RESULT_HANDLED;
invalid:
	send_reply(adapter,
		   dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS,
					  "Invalid agent arguments"));
	return DBUS_HANDLER_RESULT_HANDLED;
rejected:
	send_reply(adapter,
		   dbus_message_new_error(message, "org.bluez.Error.Rejected",
					  "No pairing interface available"));
	return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = {
	.message_function = agent_message,
};

/* ---- Signals ---- */

static bool lists_adapter(DBusMessageIter *interfaces)
{
	DBusMessageIter item;

	if (dbus_message_iter_get_arg_type(interfaces) != DBUS_TYPE_ARRAY)
		return false;
	dbus_message_iter_recurse(interfaces, &item);
	for (; dbus_message_iter_get_arg_type(&item) != DBUS_TYPE_INVALID;
	     dbus_message_iter_next(&item)) {
		DBusMessageIter name = item;
		const char *text;

		/* InterfacesAdded has dictionary entries, Removed strings. */
		if (dbus_message_iter_get_arg_type(&item) ==
		    DBUS_TYPE_DICT_ENTRY)
			dbus_message_iter_recurse(&item, &name);
		if (dbus_message_iter_get_arg_type(&name) != DBUS_TYPE_STRING)
			continue;
		dbus_message_iter_get_basic(&name, &text);
		if (!strcmp(text, BLUEZ_ADAPTER) ||
		    !strcmp(text, BLUEZ_DEVICE) || !strcmp(text, BLUEZ_BATTERY))
			return true;
	}
	return false;
}

static void set_owner(struct fplinux_bluez_adapter *adapter, const char *owner)
{
	bool requested = adapter->mutation != NULL;

	if (!strcmp(adapter->owner, owner))
		return;
	cancel_call(&adapter->snapshot);
	cancel_call(&adapter->mutation);
	cancel_call(&adapter->agent_call);
	clear_agent_request(adapter, NULL);
	adapter->agent_registered = false;
	adapter->agent_ready = false;
	adapter->agent_default_pending = false;
	adapter->close_pairing = false;
	adapter->discovery_owned = false;
	snprintf(adapter->owner, sizeof(adapter->owner), "%s", owner);
	memset(&adapter->state, 0, sizeof(adapter->state));
	if (requested)
		end_operation(adapter, ENODEV);
	changed(adapter);
	if (owner[0]) {
		query_adapters(adapter);
		register_agent(adapter);
	}
}

static void owner_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;
	const char *owner;

	(void)call;
	reply = take_reply(&adapter->owner_query, NULL);
	if (reply && dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING,
					   &owner, DBUS_TYPE_INVALID))
		set_owner(adapter, owner);
	if (reply)
		dbus_message_unref(reply);
}

static void owner_changed(struct fplinux_bluez_adapter *adapter,
			  DBusMessage *message)
{
	const char *name, *old_owner, *new_owner;

	if (!dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &old_owner,
				   DBUS_TYPE_STRING, &new_owner,
				   DBUS_TYPE_INVALID) ||
	    strcmp(name, BLUEZ_NAME))
		return;
	cancel_call(&adapter->owner_query);
	set_owner(adapter, new_owner);
}

static void interfaces_changed(struct fplinux_bluez_adapter *adapter,
			       DBusMessage *message)
{
	DBusMessageIter arguments;

	if (dbus_message_iter_init(message, &arguments) &&
	    dbus_message_iter_next(&arguments) && lists_adapter(&arguments))
		query_adapters(adapter);
}

static void properties_changed(struct fplinux_bluez_adapter *adapter,
			       DBusMessage *message)
{
	DBusMessageIter arguments;
	const char *interface;
	const char *path = dbus_message_get_path(message);
	struct inventory *state;
	struct fplinux_bluez_device *device = NULL;

	if (!fplinux_bluez_adapter_present(adapter) ||
	    !dbus_message_has_signature(message, "sa{sv}as") ||
	    !dbus_message_iter_init(message, &arguments) ||
	    dbus_message_iter_get_arg_type(&arguments) != DBUS_TYPE_STRING)
		return;
	dbus_message_iter_get_basic(&arguments, &interface);
	if ((strcmp(interface, BLUEZ_ADAPTER) &&
	     strcmp(interface, BLUEZ_DEVICE) &&
	     strcmp(interface, BLUEZ_BATTERY)) ||
	    !dbus_message_iter_next(&arguments))
		return;
	if (!strcmp(interface, BLUEZ_ADAPTER) &&
	    strcmp(path, adapter->state.path))
		return;
	state = malloc(sizeof(*state));
	if (!state)
		return;
	*state = adapter->state;
	if (!strcmp(interface, BLUEZ_ADAPTER) && !strcmp(path, state->path)) {
		read_properties(&arguments, state, NULL, interface, NULL);
	} else {
		for (size_t i = 0; i < state->device_count; ++i)
			if (!strcmp(path, state->devices[i].path))
				device = &state->devices[i];
		if (device)
			read_properties(&arguments, state, device, interface,
					NULL);
	}
	/* Invalidations require authoritative values, rather than old fields. */
	if (dbus_message_iter_next(&arguments)) {
		DBusMessageIter invalidated;

		dbus_message_iter_recurse(&arguments, &invalidated);
		if (dbus_message_iter_get_arg_type(&invalidated) !=
		    DBUS_TYPE_INVALID)
			query_adapters(adapter);
	}
	replace_inventory(adapter, state);
	free(state);
}

static DBusHandlerResult filter(DBusConnection *connection,
				DBusMessage *message, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	(void)connection;
	if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL,
				   "Disconnected"))
		adapter->lost = true;
	else if (dbus_message_has_sender(message, DBUS_SERVICE_DBUS) &&
		 dbus_message_is_signal(message, DBUS_INTERFACE_DBUS,
					"NameOwnerChanged"))
		owner_changed(adapter, message);
	else if (!adapter->owner[0] ||
		 !dbus_message_has_sender(message, adapter->owner))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	else if (dbus_message_is_signal(message, OBJECT_MANAGER,
					"InterfacesAdded") ||
		 dbus_message_is_signal(message, OBJECT_MANAGER,
					"InterfacesRemoved"))
		interfaces_changed(adapter, message);
	else if (dbus_message_is_signal(message, PROPERTIES,
					"PropertiesChanged"))
		properties_changed(adapter, message);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- Connection ---- */

static void disconnect(struct fplinux_bluez_adapter *adapter)
{
	DBusConnection *connection = adapter->connection;
	bool was_present = fplinux_bluez_adapter_present(adapter);
	bool requested = adapter->mutation != NULL;

	if (!connection)
		return;
	cancel_call(&adapter->hello);
	cancel_call(&adapter->owner_query);
	cancel_call(&adapter->snapshot);
	cancel_call(&adapter->mutation);
	cancel_call(&adapter->agent_call);
	clear_agent_request(adapter, NULL);
	dbus_connection_unregister_object_path(connection, AGENT_PATH);
	/* Removing the functions hands every watch and timeout back first. */
	dbus_connection_set_watch_functions(connection, NULL, NULL, NULL, NULL,
					    NULL);
	dbus_connection_set_timeout_functions(connection, NULL, NULL, NULL,
					      NULL, NULL);
	dbus_connection_remove_filter(connection, filter, adapter);
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	adapter->connection = NULL;
	adapter->lost = false;
	memset(&adapter->state, 0, sizeof(adapter->state));
	adapter->owner[0] = '\0';
	adapter->discovery_owned = false;
	adapter->agent_registered = false;
	adapter->agent_ready = false;
	adapter->agent_default_pending = false;
	adapter->close_pairing = false;
	if (requested)
		end_operation(adapter, ENODEV);
	if (was_present)
		changed(adapter);
}

static bool add_match(struct fplinux_bluez_adapter *adapter, const char *rule)
{
	DBusMessage *message =
		dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
					     DBUS_INTERFACE_DBUS, "AddMatch");
	bool queued = message &&
		      dbus_message_append_args(message, DBUS_TYPE_STRING, &rule,
					       DBUS_TYPE_INVALID);

	/* The bus reports a malformed rule; a valid one needs no answer. */
	if (queued) {
		dbus_message_set_no_reply(message, TRUE);
		queued = dbus_connection_send(adapter->connection, message,
					      NULL);
	}
	if (message)
		dbus_message_unref(message);
	return queued;
}

void fplinux_bluez_adapter_connect(struct fplinux_bluez_adapter *adapter)
{
	const char *address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	const char *name = BLUEZ_NAME;
	DBusMessage *message;
	DBusError error;

	if (adapter->connection)
		return;
	if (!address || !address[0])
		address = SYSTEM_BUS_ADDRESS;
	if (!dbus_threads_init_default())
		return;
	dbus_error_init(&error);
	/* A private connection authenticates and registers without waiting. */
	adapter->connection = dbus_connection_open_private(address, &error);
	dbus_error_free(&error);
	if (!adapter->connection)
		return;
	dbus_connection_set_exit_on_disconnect(adapter->connection, FALSE);
	if (!dbus_connection_set_watch_functions(adapter->connection, add_watch,
						 remove_watch, toggle_watch,
						 adapter, NULL) ||
	    !dbus_connection_set_timeout_functions(
		    adapter->connection, add_timeout, remove_timeout,
		    toggle_timeout, adapter, NULL) ||
	    !dbus_connection_add_filter(adapter->connection, filter, adapter,
					NULL) ||
	    !dbus_connection_register_object_path(
		    adapter->connection, AGENT_PATH, &agent_vtable, adapter) ||
	    !send_call(adapter,
		       dbus_message_new_method_call(
			       DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
			       DBUS_INTERFACE_DBUS, "Hello"),
		       REQUEST_TIMEOUT_MS, hello_done, &adapter->hello) ||
	    !add_match(adapter,
		       "type='signal',sender='" DBUS_SERVICE_DBUS
		       "',interface='" DBUS_INTERFACE_DBUS
		       "',member='NameOwnerChanged',arg0='" BLUEZ_NAME "'") ||
	    !add_match(adapter, "type='signal',sender='" BLUEZ_NAME
				"',interface='" OBJECT_MANAGER "'") ||
	    !add_match(adapter, "type='signal',sender='" BLUEZ_NAME
				"',interface='" PROPERTIES
				"',member='PropertiesChanged'")) {
		disconnect(adapter);
		return;
	}
	message = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
					       DBUS_PATH_DBUS,
					       DBUS_INTERFACE_DBUS,
					       "GetNameOwner");
	if (!message || !dbus_message_append_args(message, DBUS_TYPE_STRING,
						  &name, DBUS_TYPE_INVALID)) {
		if (message)
			dbus_message_unref(message);
		disconnect(adapter);
		return;
	}
	if (!send_call(adapter, message, REQUEST_TIMEOUT_MS, owner_done,
		       &adapter->owner_query))
		disconnect(adapter);
}

static void handle_socket(struct fplinux_bluez_adapter *adapter, int fd,
			  uint32_t events)
{
	unsigned int ready = 0;

	if (events & EPOLLIN)
		ready |= DBUS_WATCH_READABLE;
	if (events & EPOLLOUT)
		ready |= DBUS_WATCH_WRITABLE;
	if (events & EPOLLERR)
		ready |= DBUS_WATCH_ERROR;
	if (events & EPOLLHUP)
		ready |= DBUS_WATCH_HANGUP;
	/* Handling one watch may remove the others. */
	for (size_t i = 0; i < WATCH_COUNT && adapter->connection; ++i) {
		DBusWatch *watch = adapter->watches[i];
		unsigned int wanted;

		if (!watch || dbus_watch_get_unix_fd(watch) != fd ||
		    !dbus_watch_get_enabled(watch))
			continue;
		wanted = dbus_watch_get_flags(watch) | DBUS_WATCH_ERROR |
			 DBUS_WATCH_HANGUP;
		if (ready & wanted)
			dbus_watch_handle(watch, ready & wanted);
	}
}

void fplinux_bluez_adapter_dispatch(struct fplinux_bluez_adapter *adapter)
{
	struct epoll_event events[EVENT_COUNT];
	int count = epoll_wait(adapter->fd, events, EVENT_COUNT, 0);

	for (int i = 0; i < count && adapter->connection; ++i) {
		uint64_t data = events[i].data.u64;

		if (data & TIMEOUT_TAG) {
			struct timeout_slot *slot =
				&adapter->timeouts[data & ~TIMEOUT_TAG];
			uint64_t expirations;

			/* A slot reused within this round has not expired. */
			if (slot->timeout &&
			    read(slot->fd, &expirations, sizeof(expirations)) ==
				    (ssize_t)sizeof(expirations))
				dbus_timeout_handle(slot->timeout);
		} else {
			handle_socket(adapter, (int)data, events[i].events);
		}
	}
	while (adapter->connection && !adapter->lost &&
	       dbus_connection_dispatch(adapter->connection) ==
		       DBUS_DISPATCH_DATA_REMAINS)
		;
	if (adapter->connection &&
	    (adapter->lost ||
	     !dbus_connection_get_is_connected(adapter->connection)))
		disconnect(adapter);
}

/* ---- Public state ---- */

struct fplinux_bluez_adapter *fplinux_bluez_adapter_open(
	const struct fplinux_bluez_adapter_listener *listener)
{
	struct fplinux_bluez_adapter *adapter = calloc(1, sizeof(*adapter));

	if (!adapter)
		return NULL;
	for (size_t i = 0; i < TIMEOUT_COUNT; ++i)
		adapter->timeouts[i].fd = -1;
	if (listener)
		adapter->listener = *listener;
	adapter->fd = epoll_create1(EPOLL_CLOEXEC);
	if (adapter->fd < 0) {
		int saved = errno;

		free(adapter);
		errno = saved;
		return NULL;
	}
	return adapter;
}

void fplinux_bluez_adapter_close(struct fplinux_bluez_adapter *adapter)
{
	if (!adapter)
		return;
	/* Closing reports nothing to a listener that is going away. */
	adapter->listener = (struct fplinux_bluez_adapter_listener){ 0 };
	disconnect(adapter);
	close(adapter->fd);
	free(adapter);
}

int fplinux_bluez_adapter_fd(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->fd;
}

bool fplinux_bluez_adapter_present(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->connection && adapter->state.adapters == 1;
}

bool fplinux_bluez_adapter_powered(const struct fplinux_bluez_adapter *adapter)
{
	return fplinux_bluez_adapter_present(adapter) && adapter->state.powered;
}

bool fplinux_bluez_adapter_connectable(
	const struct fplinux_bluez_adapter *adapter)
{
	return fplinux_bluez_adapter_present(adapter) &&
	       adapter->state.connectable;
}

bool fplinux_bluez_adapter_pairable(const struct fplinux_bluez_adapter *adapter)
{
	return fplinux_bluez_adapter_present(adapter) &&
	       adapter->state.pairable;
}

const char *
fplinux_bluez_adapter_address(const struct fplinux_bluez_adapter *adapter)
{
	return fplinux_bluez_adapter_present(adapter) ? adapter->state.address :
							"";
}

const char *
fplinux_bluez_adapter_owner(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->owner;
}

bool fplinux_bluez_adapter_pending(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->mutation != NULL;
}

static bool can_mutate(struct fplinux_bluez_adapter *adapter,
		       const char *device)
{
	adapter->error =
		!fplinux_bluez_adapter_present(adapter) ? ENODEV :
		adapter->mutation			? EBUSY :
		device && !fplinux_bluez_adapter_device(adapter, device) ?
				    ENODEV :
				    0;
	return !adapter->error;
}

static bool start_mutation(struct fplinux_bluez_adapter *adapter,
			   DBusMessage *message, enum operation operation,
			   const char *device, int timeout_ms)
{
	if (timeout_ms <= 0) {
		if (message)
			dbus_message_unref(message);
		adapter->error = EINVAL;
		return false;
	}
	if (!send_call(adapter, message, timeout_ms, mutation_done,
		       &adapter->mutation)) {
		adapter->error = ENOMEM;
		return false;
	}
	adapter->error = 0;
	adapter->operation = operation;
	adapter->timeout_ms = timeout_ms;
	snprintf(adapter->operation_device, sizeof(adapter->operation_device),
		 "%s", device ? device : "");
	return true;
}

bool fplinux_bluez_adapter_set_powered(struct fplinux_bluez_adapter *adapter,
				       bool powered, int timeout_ms)
{
	dbus_bool_t value = powered;

	if (!can_mutate(adapter, NULL))
		return false;
	adapter->requested = powered;
	adapter->policy_step = 0;
	return start_mutation(adapter,
			      property_message(adapter, adapter->state.path,
					       BLUEZ_ADAPTER, "Powered",
					       DBUS_TYPE_BOOLEAN, &value),
			      OP_POWER, NULL, timeout_ms);
}

const char *
fplinux_bluez_adapter_name(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->state.name;
}

enum fplinux_bluez_visibility
fplinux_bluez_adapter_visibility(const struct fplinux_bluez_adapter *adapter)
{
	if (!fplinux_bluez_adapter_powered(adapter) ||
	    !adapter->state.discoverable)
		return FPLINUX_BLUEZ_HIDDEN;
	return adapter->state.discoverable_timeout ?
		       FPLINUX_BLUEZ_VISIBLE_TIMED :
		       FPLINUX_BLUEZ_VISIBLE_ALWAYS;
}

bool fplinux_bluez_adapter_discovering(
	const struct fplinux_bluez_adapter *adapter)
{
	return adapter->state.discovering;
}

bool fplinux_bluez_adapter_discovery_owned(
	const struct fplinux_bluez_adapter *adapter)
{
	return adapter->discovery_owned;
}

int fplinux_bluez_adapter_error(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->error;
}

size_t
fplinux_bluez_adapter_device_count(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->state.device_count;
}

const struct fplinux_bluez_device *
fplinux_bluez_adapter_device_at(const struct fplinux_bluez_adapter *adapter,
				size_t index)
{
	return index < adapter->state.device_count ?
		       &adapter->state.devices[index] :
		       NULL;
}

const struct fplinux_bluez_device *
fplinux_bluez_adapter_device(const struct fplinux_bluez_adapter *adapter,
			     const char *path)
{
	if (!path)
		return NULL;
	for (size_t i = 0; i < adapter->state.device_count; ++i)
		if (!strcmp(path, adapter->state.devices[i].path))
			return &adapter->state.devices[i];
	return NULL;
}

static bool set_text(struct fplinux_bluez_adapter *adapter, const char *path,
		     const char *text, int timeout_ms)
{
	if (!can_mutate(adapter, path))
		return false;
	if (!text || strlen(text) >= FPLINUX_BLUEZ_NAME_BYTES ||
	    !dbus_validate_utf8(text, NULL)) {
		adapter->error = EINVAL;
		return false;
	}
	return start_mutation(
		adapter,
		property_message(adapter, path ? path : adapter->state.path,
				 path ? BLUEZ_DEVICE : BLUEZ_ADAPTER, "Alias",
				 DBUS_TYPE_STRING, &text),
		OP_CHANGE, path, timeout_ms);
}

bool fplinux_bluez_adapter_set_name(struct fplinux_bluez_adapter *adapter,
				    const char *name, int timeout_ms)
{
	return set_text(adapter, NULL, name, timeout_ms);
}

bool fplinux_bluez_adapter_set_alias(struct fplinux_bluez_adapter *adapter,
				     const char *path, const char *alias,
				     int timeout_ms)
{
	if (!path) {
		adapter->error = EINVAL;
		return false;
	}
	return set_text(adapter, path, alias, timeout_ms);
}

bool fplinux_bluez_adapter_set_visibility(
	struct fplinux_bluez_adapter *adapter,
	enum fplinux_bluez_visibility visibility, int timeout_ms)
{
	const struct mode_change *steps;

	if (!can_mutate(adapter, NULL))
		return false;
	if (visibility < FPLINUX_BLUEZ_HIDDEN ||
	    visibility > FPLINUX_BLUEZ_VISIBLE_ALWAYS) {
		adapter->error = EINVAL;
		return false;
	}
	if (!fplinux_bluez_adapter_powered(adapter)) {
		adapter->error = ENETDOWN;
		return false;
	}
	adapter->visibility = visibility;
	adapter->policy_step = 1;
	steps = visibility == FPLINUX_BLUEZ_HIDDEN ? hide_adapter :
						     show_adapter;
	return start_mutation(adapter, mode_message(adapter, &steps[0]),
			      OP_VISIBILITY, NULL, timeout_ms);
}

bool fplinux_bluez_adapter_set_discovery(struct fplinux_bluez_adapter *adapter,
					 bool discover, int timeout_ms)
{
	if (!can_mutate(adapter, NULL))
		return false;
	if (discover == adapter->discovery_owned) {
		adapter->error = EALREADY;
		return false;
	}
	return start_mutation(
		adapter,
		dbus_message_new_method_call(
			adapter->owner, adapter->state.path, BLUEZ_ADAPTER,
			discover ? "StartDiscovery" : "StopDiscovery"),
		discover ? OP_DISCOVERY_START : OP_DISCOVERY_STOP, NULL,
		timeout_ms);
}

static bool device_method(struct fplinux_bluez_adapter *adapter,
			  const char *path, const char *method,
			  const char *uuid, int timeout_ms)
{
	DBusMessage *message;

	if (!path || !can_mutate(adapter, path)) {
		if (!path)
			adapter->error = EINVAL;
		return false;
	}
	message = dbus_message_new_method_call(adapter->owner, path,
					       BLUEZ_DEVICE, method);
	if (uuid && message &&
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &uuid,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	return start_mutation(adapter, message,
			      !strcmp(method, "Pair") ? OP_PAIR : OP_CHANGE,
			      path, timeout_ms);
}

bool fplinux_bluez_adapter_pair(struct fplinux_bluez_adapter *adapter,
				const char *path, int timeout_ms)
{
	if (!adapter->agent_registered) {
		adapter->error = EACCES;
		return false;
	}
	return device_method(adapter, path, "Pair", NULL, timeout_ms);
}

bool fplinux_bluez_adapter_connect_device(struct fplinux_bluez_adapter *adapter,
					  const char *path, int timeout_ms)
{
	return device_method(adapter, path, "Connect", NULL, timeout_ms);
}

bool fplinux_bluez_adapter_disconnect_device(
	struct fplinux_bluez_adapter *adapter, const char *path, int timeout_ms)
{
	return device_method(adapter, path, "Disconnect", NULL, timeout_ms);
}

bool fplinux_bluez_adapter_connect_profile(struct fplinux_bluez_adapter *adapter,
					   const char *path, const char *uuid,
					   int timeout_ms)
{
	if (!uuid || strlen(uuid) != 36 || !dbus_validate_utf8(uuid, NULL)) {
		adapter->error = EINVAL;
		return false;
	}
	return device_method(adapter, path, "ConnectProfile", uuid, timeout_ms);
}

bool fplinux_bluez_adapter_set_trusted(struct fplinux_bluez_adapter *adapter,
				       const char *path, bool trusted,
				       int timeout_ms)
{
	dbus_bool_t value = trusted;

	if (!path || !can_mutate(adapter, path)) {
		if (!path)
			adapter->error = EINVAL;
		return false;
	}
	return start_mutation(adapter,
			      property_message(adapter, path, BLUEZ_DEVICE,
					       "Trusted", DBUS_TYPE_BOOLEAN,
					       &value),
			      OP_CHANGE, path, timeout_ms);
}

bool fplinux_bluez_adapter_remove_device(struct fplinux_bluez_adapter *adapter,
					 const char *path, int timeout_ms)
{
	DBusMessage *message;

	if (!path || !can_mutate(adapter, path)) {
		if (!path)
			adapter->error = EINVAL;
		return false;
	}
	message = dbus_message_new_method_call(adapter->owner,
					       adapter->state.path,
					       BLUEZ_ADAPTER, "RemoveDevice");
	if (message && !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH,
						 &path, DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	return start_mutation(adapter, message, OP_CHANGE, NULL, timeout_ms);
}

bool fplinux_bluez_adapter_cancel_pairing(struct fplinux_bluez_adapter *adapter)
{
	const char *path = adapter->operation == OP_PAIR ?
				   adapter->operation_device :
				   adapter->agent_request.device;
	DBusMessage *message;
	bool queued;

	if (!path[0] || !adapter->connection || !adapter->owner[0]) {
		adapter->error = ENOENT;
		return false;
	}
	message = dbus_message_new_method_call(adapter->owner, path,
					       BLUEZ_DEVICE, "CancelPairing");
	if (!message) {
		adapter->error = ENOMEM;
		return false;
	}
	dbus_message_set_no_reply(message, TRUE);
	queued = dbus_connection_send(adapter->connection, message, NULL);
	dbus_message_unref(message);
	if (queued) {
		abort_pair(adapter);
		changed(adapter);
	}
	return queued;
}

bool fplinux_bluez_adapter_set_agent_enabled(
	struct fplinux_bluez_adapter *adapter, bool enabled)
{
	bool was_enabled = adapter->agent_enabled;

	adapter->agent_enabled = enabled;
	if (enabled) {
		adapter->close_pairing = false;
		register_agent(adapter);
		return !adapter->owner[0] || adapter->agent_registered ||
		       adapter->agent_call ||
		       adapter->operation == OP_PAIRABLE_OFF;
	}
	if (!was_enabled)
		return true;
	if (adapter->operation == OP_PAIR || adapter->agent_request.device[0])
		fplinux_bluez_adapter_cancel_pairing(adapter);
	abort_pair(adapter);
	cancel_call(&adapter->agent_call);
	if (adapter->connection && adapter->owner[0]) {
		DBusMessage *message =
			agent_manager_message(adapter, "UnregisterAgent");

		if (message) {
			dbus_message_set_no_reply(message, TRUE);
			dbus_connection_send(adapter->connection, message,
					     NULL);
			dbus_message_unref(message);
		}
	}
	adapter->agent_registered = false;
	adapter->agent_ready = false;
	adapter->agent_default_pending = false;
	adapter->close_pairing = true;
	close_pairing_if_idle(adapter);
	changed(adapter);
	return true;
}

bool fplinux_bluez_adapter_agent_ready(
	const struct fplinux_bluez_adapter *adapter)
{
	return adapter->agent_ready;
}

const struct fplinux_bluez_agent_request *
fplinux_bluez_adapter_agent_request(const struct fplinux_bluez_adapter *adapter)
{
	return &adapter->agent_request;
}

bool fplinux_bluez_adapter_agent_reply(struct fplinux_bluez_adapter *adapter,
				       uint64_t serial, bool allow,
				       const char *code)
{
	DBusMessage *reply;
	dbus_uint32_t passkey = 0;

	if (!serial || serial != adapter->agent_request.serial ||
	    !adapter->agent_message) {
		adapter->error = ENOENT;
		return false;
	}
	if (allow && adapter->agent_request.kind == FPLINUX_BLUEZ_AGENT_PIN &&
	    (!code || !code[0] || strlen(code) > 16 ||
	     !dbus_validate_utf8(code, NULL))) {
		adapter->error = EINVAL;
		return false;
	}
	if (allow &&
	    adapter->agent_request.kind == FPLINUX_BLUEZ_AGENT_PASSKEY) {
		if (!code || !code[0] || strlen(code) > 6) {
			adapter->error = EINVAL;
			return false;
		}
		for (const char *digit = code; *digit; ++digit) {
			if (*digit < '0' || *digit > '9') {
				adapter->error = EINVAL;
				return false;
			}
			passkey = passkey * 10 + (unsigned int)(*digit - '0');
		}
	}
	reply = allow ? dbus_message_new_method_return(adapter->agent_message) :
			dbus_message_new_error(adapter->agent_message,
					       "org.bluez.Error.Rejected",
					       "Pairing declined");
	if (!reply) {
		adapter->error = ENOMEM;
		return false;
	}
	if (allow &&
	    ((adapter->agent_request.kind == FPLINUX_BLUEZ_AGENT_PIN &&
	      !dbus_message_append_args(reply, DBUS_TYPE_STRING, &code,
					DBUS_TYPE_INVALID)) ||
	     (adapter->agent_request.kind == FPLINUX_BLUEZ_AGENT_PASSKEY &&
	      !dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey,
					DBUS_TYPE_INVALID)))) {
		dbus_message_unref(reply);
		adapter->error = ENOMEM;
		return false;
	}
	if (!dbus_connection_send(adapter->connection, reply, NULL)) {
		dbus_message_unref(reply);
		adapter->error = ENOMEM;
		return false;
	}
	dbus_message_unref(reply);
	clear_agent_request(adapter, NULL);
	adapter->error = 0;
	changed(adapter);
	return true;
}
