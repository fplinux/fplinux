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
#define PATH_BYTES 256

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
	DBusPendingCall *snapshot;
	DBusPendingCall *power;
	bool requested;
	/* BlueZ's adapters by the latest snapshot; the path of the only one. */
	unsigned int adapters;
	char path[PATH_BYTES];
	bool powered;
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
static DBusMessage *take_reply(DBusPendingCall **pending, const char *request)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(*pending);

	dbus_pending_call_unref(*pending);
	*pending = NULL;
	if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *text = NULL;

		dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &text,
				      DBUS_TYPE_INVALID);
		/* A missing BlueZ is an expected answer to the snapshot. */
		if (strcmp(dbus_message_get_error_name(reply),
			   DBUS_ERROR_SERVICE_UNKNOWN) &&
		    strcmp(dbus_message_get_error_name(reply),
			   DBUS_ERROR_NAME_HAS_NO_OWNER))
			fprintf(stderr,
				"fplinux-bluez-adapter: %s failed: %s%s%s\n",
				request, dbus_message_get_error_name(reply),
				text ? ": " : "", text ? text : "");
		dbus_message_unref(reply);
		return NULL;
	}
	return reply;
}

static void set_adapters(struct fplinux_bluez_adapter *adapter,
			 unsigned int adapters, const char *path, bool powered)
{
	bool was_present = fplinux_bluez_adapter_present(adapter);
	bool was_powered = adapter->powered;
	bool moved = strcmp(adapter->path, adapters == 1 ? path : "");

	adapter->adapters = adapters;
	snprintf(adapter->path, sizeof(adapter->path), "%s",
		 adapters == 1 ? path : "");
	adapter->powered = adapters == 1 && powered;
	/* A request for an adapter that is gone cannot be answered. */
	if (adapter->power && (adapters != 1 || moved)) {
		cancel_call(&adapter->power);
		finished(adapter, false);
	}
	if (was_present != fplinux_bluez_adapter_present(adapter) ||
	    was_powered != adapter->powered || moved)
		changed(adapter);
}

/* Reads Powered from an a{sv} property dictionary. */
static void read_powered(DBusMessageIter *properties, bool *powered)
{
	DBusMessageIter entry;

	if (dbus_message_iter_get_arg_type(properties) != DBUS_TYPE_ARRAY)
		return;
	dbus_message_iter_recurse(properties, &entry);
	for (; dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(&entry)) {
		DBusMessageIter field, value;
		const char *name;
		dbus_bool_t on;

		dbus_message_iter_recurse(&entry, &field);
		if (dbus_message_iter_get_arg_type(&field) != DBUS_TYPE_STRING)
			continue;
		dbus_message_iter_get_basic(&field, &name);
		if (strcmp(name, "Powered") ||
		    !dbus_message_iter_next(&field) ||
		    dbus_message_iter_get_arg_type(&field) != DBUS_TYPE_VARIANT)
			continue;
		dbus_message_iter_recurse(&field, &value);
		if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_BOOLEAN)
			continue;
		dbus_message_iter_get_basic(&value, &on);
		*powered = on;
	}
}

/* Counts the Adapter1 objects of an a{oa{sa{sv}}} snapshot. */
static void read_snapshot(struct fplinux_bluez_adapter *adapter,
			  DBusMessage *reply)
{
	DBusMessageIter objects, object;
	unsigned int adapters = 0;
	const char *first = "";
	bool powered = false;

	if (reply && dbus_message_iter_init(reply, &objects) &&
	    dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_ARRAY)
		dbus_message_iter_recurse(&objects, &object);
	else
		reply = NULL;
	for (; reply &&
	       dbus_message_iter_get_arg_type(&object) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(&object)) {
		DBusMessageIter field, interfaces;
		const char *path;

		dbus_message_iter_recurse(&object, &field);
		if (dbus_message_iter_get_arg_type(&field) !=
		    DBUS_TYPE_OBJECT_PATH)
			continue;
		dbus_message_iter_get_basic(&field, &path);
		if (!dbus_message_iter_next(&field) ||
		    dbus_message_iter_get_arg_type(&field) != DBUS_TYPE_ARRAY)
			continue;
		dbus_message_iter_recurse(&field, &interfaces);
		for (; dbus_message_iter_get_arg_type(&interfaces) ==
		       DBUS_TYPE_DICT_ENTRY;
		     dbus_message_iter_next(&interfaces)) {
			DBusMessageIter interface;
			const char *name;

			dbus_message_iter_recurse(&interfaces, &interface);
			if (dbus_message_iter_get_arg_type(&interface) !=
			    DBUS_TYPE_STRING)
				continue;
			dbus_message_iter_get_basic(&interface, &name);
			if (strcmp(name, BLUEZ_ADAPTER))
				continue;
			if (!adapters++) {
				first = path;
				dbus_message_iter_next(&interface);
				read_powered(&interface, &powered);
			}
		}
	}
	set_adapters(adapter, adapters, first, powered);
}

static void snapshot_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;

	(void)call;
	reply = take_reply(&adapter->snapshot, "query");
	read_snapshot(adapter, reply);
	if (reply)
		dbus_message_unref(reply);
}

/* A newer snapshot replaces one still on its way. */
static void query_adapters(struct fplinux_bluez_adapter *adapter)
{
	cancel_call(&adapter->snapshot);
	if (!send_call(adapter,
		       dbus_message_new_method_call(BLUEZ_NAME, "/",
						    OBJECT_MANAGER,
						    "GetManagedObjects"),
		       REQUEST_TIMEOUT_MS, snapshot_done, &adapter->snapshot))
		set_adapters(adapter, 0, "", false);
}

static void power_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;

	(void)call;
	reply = take_reply(&adapter->power, "power change");
	if (reply) {
		dbus_message_unref(reply);
		/* BlueZ answers once the adapter has the new power state. */
		if (adapter->powered != adapter->requested) {
			adapter->powered = adapter->requested;
			changed(adapter);
		}
	}
	finished(adapter, reply != NULL);
}

static void hello_done(DBusPendingCall *call, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;
	DBusMessage *reply;
	const char *name;

	(void)call;
	reply = take_reply(&adapter->hello, "bus registration");
	if (reply && dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_INVALID))
		dbus_bus_set_unique_name(adapter->connection, name);
	else
		adapter->lost = true;
	if (reply)
		dbus_message_unref(reply);
}

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
		if (!strcmp(text, BLUEZ_ADAPTER))
			return true;
	}
	return false;
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
	if (new_owner[0]) {
		query_adapters(adapter);
		return;
	}
	cancel_call(&adapter->snapshot);
	set_adapters(adapter, 0, "", false);
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
	bool powered = adapter->powered;

	if (!fplinux_bluez_adapter_present(adapter) ||
	    strcmp(dbus_message_get_path(message), adapter->path) ||
	    !dbus_message_iter_init(message, &arguments) ||
	    dbus_message_iter_get_arg_type(&arguments) != DBUS_TYPE_STRING)
		return;
	dbus_message_iter_get_basic(&arguments, &interface);
	if (strcmp(interface, BLUEZ_ADAPTER) ||
	    !dbus_message_iter_next(&arguments))
		return;
	read_powered(&arguments, &powered);
	if (powered != adapter->powered) {
		adapter->powered = powered;
		changed(adapter);
	}
}

static DBusHandlerResult filter(DBusConnection *connection,
				DBusMessage *message, void *data)
{
	struct fplinux_bluez_adapter *adapter = data;

	(void)connection;
	if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL,
				   "Disconnected"))
		adapter->lost = true;
	else if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS,
					"NameOwnerChanged"))
		owner_changed(adapter, message);
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
	bool requested = adapter->power != NULL;

	if (!connection)
		return;
	cancel_call(&adapter->hello);
	cancel_call(&adapter->snapshot);
	cancel_call(&adapter->power);
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
	adapter->adapters = 0;
	adapter->path[0] = '\0';
	adapter->powered = false;
	if (requested)
		finished(adapter, false);
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
	DBusError error;

	if (adapter->connection)
		return;
	if (!address || !address[0])
		address = SYSTEM_BUS_ADDRESS;
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
	    !add_match(adapter,
		       "type='signal',sender='" BLUEZ_NAME
		       "',interface='" PROPERTIES
		       "',member='PropertiesChanged',arg0='" BLUEZ_ADAPTER
		       "'")) {
		disconnect(adapter);
		return;
	}
	query_adapters(adapter);
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
	return adapter->connection && adapter->adapters == 1;
}

bool fplinux_bluez_adapter_powered(const struct fplinux_bluez_adapter *adapter)
{
	return fplinux_bluez_adapter_present(adapter) && adapter->powered;
}

bool fplinux_bluez_adapter_pending(const struct fplinux_bluez_adapter *adapter)
{
	return adapter->power != NULL;
}

bool fplinux_bluez_adapter_set_powered(struct fplinux_bluez_adapter *adapter,
				       bool powered, int timeout_ms)
{
	const char *interface = BLUEZ_ADAPTER;
	const char *property = "Powered";
	dbus_bool_t value = powered;
	DBusMessage *message;
	DBusMessageIter arguments, variant;

	if (!fplinux_bluez_adapter_present(adapter) || adapter->power)
		return false;
	message = dbus_message_new_method_call(BLUEZ_NAME, adapter->path,
					       PROPERTIES, "Set");
	if (!message)
		return false;
	dbus_message_iter_init_append(message, &arguments);
	if (!dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					    &interface) ||
	    !dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING,
					    &property) ||
	    !dbus_message_iter_open_container(&arguments, DBUS_TYPE_VARIANT,
					      DBUS_TYPE_BOOLEAN_AS_STRING,
					      &variant)) {
		dbus_message_unref(message);
		return false;
	}
	if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN,
					    &value) ||
	    !dbus_message_iter_close_container(&arguments, &variant)) {
		dbus_message_iter_abandon_container(&arguments, &variant);
		dbus_message_unref(message);
		return false;
	}
	if (!send_call(adapter, message, timeout_ms, power_done,
		       &adapter->power))
		return false;
	adapter->requested = powered;
	return true;
}
