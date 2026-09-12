// SPDX-License-Identifier: GPL-2.0-only
/*
 * Small D-Bus service used only by test_fplinux_bluetooth.py.  It replaces
 * BlueZ and obexd at their public D-Bus boundary; it does not model either
 * daemon beyond the four client-visible cases exercised there.
 */
#define _POSIX_C_SOURCE 200809L

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OBEX_NAME "org.bluez.obex"
#define TRANSFER_PATH "/org/bluez/obex/transfer0"
#define SESSION_PATH "/org/bluez/obex/session0"
#define DEVICE_PATH "/org/bluez/hci0/dev_01_23_45_67_89_AB"

enum mode {
	SEND_FAST,
	RECEIVE,
	NETWORK_EARLY,
	NETWORK_LATE,
	NETWORK_SNAPSHOT_MISSING,
};

struct service {
	DBusConnection *connection;
	enum mode mode;
	char *agent_owner;
	char *agent_path;
	DBusPendingCall *agent_call;
	bool agent_started;
	dbus_bool_t connected;
	long long stop_at;
};

static volatile sig_atomic_t release_network;

static void on_sigusr1(int unused)
{
	(void)unused;
	release_network = 1;
}

static long long milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void fail(const char *message)
{
	fprintf(stderr, "fplinux Bluetooth test service: %s\n", message);
	exit(2);
}

static void send_reply(DBusConnection *connection, DBusMessage *reply)
{
	if (reply == NULL)
		fail("cannot construct D-Bus reply");
	if (!dbus_connection_send(connection, reply, NULL)) {
		dbus_message_unref(reply);
		fail("cannot queue D-Bus reply");
	}
	dbus_connection_flush(connection);
	dbus_message_unref(reply);
}

static void reply_empty(DBusConnection *connection, DBusMessage *message)
{
	send_reply(connection, dbus_message_new_method_return(message));
}

static void reply_string(DBusConnection *connection, DBusMessage *message,
			 const char *value)
{
	DBusMessage *reply = dbus_message_new_method_return(message);
	if (reply == NULL ||
	    !dbus_message_append_args(reply, DBUS_TYPE_STRING, &value,
				      DBUS_TYPE_INVALID)) {
		if (reply != NULL)
			dbus_message_unref(reply);
		fail("cannot construct string reply");
	}
	send_reply(connection, reply);
}

static void reply_object(DBusConnection *connection, DBusMessage *message,
			 const char *value)
{
	DBusMessage *reply = dbus_message_new_method_return(message);
	if (reply == NULL ||
	    !dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &value,
				      DBUS_TYPE_INVALID)) {
		if (reply != NULL)
			dbus_message_unref(reply);
		fail("cannot construct object-path reply");
	}
	send_reply(connection, reply);
}

static void reply_property(DBusConnection *connection, DBusMessage *message,
			   int type, const void *value)
{
	const char *signature = type == DBUS_TYPE_BOOLEAN     ? "b" :
				type == DBUS_TYPE_OBJECT_PATH ? "o" :
								"s";
	DBusMessage *reply = dbus_message_new_method_return(message);
	DBusMessageIter root, variant;
	if (reply == NULL) {
		fail("cannot construct property reply");
	}
	dbus_message_iter_init_append(reply, &root);
	if (!dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT,
					      signature, &variant) ||
	    !dbus_message_iter_append_basic(&variant, type, value) ||
	    !dbus_message_iter_close_container(&root, &variant)) {
		dbus_message_unref(reply);
		fail("cannot construct property reply");
	}
	send_reply(connection, reply);
}

static void reply_unknown_property(DBusConnection *connection,
				   DBusMessage *message)
{
	DBusMessage *reply = dbus_message_new_error(
		message, "org.freedesktop.DBus.Error.UnknownProperty",
		"property is unavailable before authorization");
	send_reply(connection, reply);
}

static void reply_unknown_object(DBusConnection *connection,
				 DBusMessage *message)
{
	DBusMessage *reply = dbus_message_new_error(
		message, "org.freedesktop.DBus.Error.UnknownObject",
		"device object disappeared after Connect");
	send_reply(connection, reply);
}

static void emit_property(DBusConnection *connection, const char *path,
			  const char *interface, const char *key, int type,
			  const void *value)
{
	const char *signature = type == DBUS_TYPE_BOOLEAN ? "b" : "s";
	DBusMessage *signal = dbus_message_new_signal(
		path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	DBusMessageIter root, array, entry, variant, invalidated;
	if (signal == NULL) {
		fail("cannot construct PropertiesChanged");
	}
	dbus_message_iter_init_append(signal, &root);
	if (!dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING,
					    &interface) ||
	    !dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}",
					      &array) ||
	    !dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
					      NULL, &entry) ||
	    !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
	    !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					      signature, &variant) ||
	    !dbus_message_iter_append_basic(&variant, type, value) ||
	    !dbus_message_iter_close_container(&entry, &variant) ||
	    !dbus_message_iter_close_container(&array, &entry) ||
	    !dbus_message_iter_close_container(&root, &array) ||
	    !dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "s",
					      &invalidated) ||
	    !dbus_message_iter_close_container(&root, &invalidated)) {
		dbus_message_unref(signal);
		fail("cannot construct PropertiesChanged");
	}
	if (strcmp(dbus_message_get_signature(signal), "sa{sv}as") != 0) {
		dbus_message_unref(signal);
		fail("PropertiesChanged has an invalid signature");
	}
	if (!dbus_connection_send(connection, signal, NULL)) {
		dbus_message_unref(signal);
		fail("cannot queue PropertiesChanged");
	}
	dbus_connection_flush(connection);
	dbus_message_unref(signal);
}

static void begin_authorization(struct service *service)
{
	const char *transfer = TRANSFER_PATH;
	DBusMessage *message = dbus_message_new_method_call(
		service->agent_owner, service->agent_path,
		"org.bluez.obex.Agent1", "AuthorizePush");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &transfer,
				      DBUS_TYPE_INVALID) ||
	    !dbus_connection_send_with_reply(service->connection, message,
					     &service->agent_call, 5000)) {
		if (message != NULL)
			dbus_message_unref(message);
		fail("cannot request receive authorization");
	}
	dbus_message_unref(message);
	service->agent_started = true;
}

static void finish_authorization(struct service *service)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(service->agent_call);
	const char *target;
	if (reply == NULL ||
	    dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &target,
				   DBUS_TYPE_INVALID)) {
		if (reply != NULL)
			dbus_message_unref(reply);
		dbus_pending_call_unref(service->agent_call);
		service->agent_call = NULL;
		return;
	}
	int file = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (file < 0 || write(file, "fixture", 7) != 7 || close(file) < 0) {
		if (file >= 0)
			close(file);
		dbus_message_unref(reply);
		fail("cannot write the authorized receive target");
	}
	dbus_message_unref(reply);
	dbus_pending_call_unref(service->agent_call);
	service->agent_call = NULL;
	const char *complete = "complete";
	emit_property(service->connection, TRANSFER_PATH,
		      "org.bluez.obex.Transfer1", "Status", DBUS_TYPE_STRING,
		      &complete);
}

static DBusHandlerResult handle_property(DBusConnection *connection,
					 DBusMessage *message,
					 struct service *service)
{
	const char *interface;
	const char *property;
	const char *path = dbus_message_get_path(message);
	if (!dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &interface,
				   DBUS_TYPE_STRING, &property,
				   DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (strcmp(interface, "org.bluez.obex.Transfer1") == 0 &&
	    strcmp(property, "Session") == 0) {
		const char *value = SESSION_PATH;
		reply_property(connection, message, DBUS_TYPE_OBJECT_PATH,
			       &value);
	} else if (strcmp(interface, "org.bluez.obex.Transfer1") == 0 &&
		   strcmp(property, "Filename") == 0) {
		reply_unknown_property(connection, message);
	} else if (strcmp(interface, "org.bluez.obex.Transfer1") == 0 &&
		   strcmp(property, "Name") == 0) {
		const char *value = "incoming.txt";
		reply_property(connection, message, DBUS_TYPE_STRING, &value);
	} else if (strcmp(interface, "org.bluez.obex.Transfer1") == 0 &&
		   strcmp(property, "Status") == 0) {
		reply_unknown_property(connection, message);
	} else if (strcmp(interface, "org.bluez.obex.Session1") == 0 &&
		   strcmp(property, "Destination") == 0) {
		const char *value = "01:23:45:67:89:AB";
		reply_property(connection, message, DBUS_TYPE_STRING, &value);
	} else if (strcmp(interface, "org.bluez.Network1") == 0 &&
		   strcmp(property, "Connected") == 0) {
		if (path == NULL || strcmp(path, DEVICE_PATH) != 0)
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		if (service->mode == NETWORK_SNAPSHOT_MISSING)
			reply_unknown_object(connection, message);
		else
			reply_property(connection, message, DBUS_TYPE_BOOLEAN,
				       &service->connected);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle(DBusConnection *connection,
				DBusMessage *message, void *data)
{
	struct service *service = data;
	if (dbus_message_is_method_call(message, "org.bluez.obex.Client1",
					"CreateSession")) {
		const char *session = SESSION_PATH;
		reply_object(connection, message, session);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(message, "org.bluez.obex.ObjectPush1",
					"SendFile")) {
		const char *complete = "complete";
		emit_property(connection, TRANSFER_PATH,
			      "org.bluez.obex.Transfer1", "Status",
			      DBUS_TYPE_STRING, &complete);
		reply_object(connection, message, TRANSFER_PATH);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(message, "org.bluez.obex.AgentManager1",
					"RegisterAgent")) {
		const char *path;
		const char *sender = dbus_message_get_sender(message);
		if (sender == NULL ||
		    !dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH,
					   &path, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		service->agent_owner = strdup(sender);
		service->agent_path = strdup(path);
		if (service->agent_owner == NULL || service->agent_path == NULL)
			fail("cannot retain agent address");
		reply_empty(connection, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(message, "org.bluez.obex.AgentManager1",
					"UnregisterAgent") ||
	    dbus_message_is_method_call(message, "org.bluez.obex.Client1",
					"RemoveSession")) {
		reply_empty(connection, message);
		service->stop_at = milliseconds() + 100;
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(
		    message, "org.freedesktop.DBus.Properties", "Get"))
		return handle_property(connection, message, service);
	if (dbus_message_is_method_call(message, "org.bluez.Network1",
					"Connect")) {
		const char *path = dbus_message_get_path(message);
		if (path == NULL || strcmp(path, DEVICE_PATH) != 0)
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		if (service->mode == NETWORK_EARLY) {
			const char *key = "Connected";
			service->connected = false;
			emit_property(connection, DEVICE_PATH,
				      "org.bluez.Network1", key,
				      DBUS_TYPE_BOOLEAN, &service->connected);
		} else {
			service->connected = true;
		}
		reply_string(connection, message, "bnep0");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_message_is_method_call(message, "org.bluez.Network1",
					"Disconnect")) {
		const char *path = dbus_message_get_path(message);
		if (path == NULL || strcmp(path, DEVICE_PATH) != 0)
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		reply_empty(connection, message);
		service->stop_at = milliseconds() + 100;
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static enum mode parse_mode(const char *argument)
{
	if (strcmp(argument, "send-fast") == 0)
		return SEND_FAST;
	if (strcmp(argument, "receive") == 0)
		return RECEIVE;
	if (strcmp(argument, "network-early") == 0)
		return NETWORK_EARLY;
	if (strcmp(argument, "network-late") == 0)
		return NETWORK_LATE;
	if (strcmp(argument, "network-snapshot-missing") == 0)
		return NETWORK_SNAPSHOT_MISSING;
	fail("unknown mode");
	return SEND_FAST;
}

static void publish_ready(const char *path)
{
	int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (file < 0 || write(file, "ready\n", 6) != 6 || close(file) < 0) {
		if (file >= 0)
			close(file);
		fail("cannot publish readiness marker");
	}
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s MODE READY_FILE\n", argv[0]);
		return 2;
	}
	struct service service = { .mode = parse_mode(argv[1]) };
	struct sigaction action = { .sa_handler = on_sigusr1 };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, NULL) < 0)
		fail("cannot install SIGUSR1 handler");
	DBusError error;
	dbus_error_init(&error);
	service.connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (service.connection == NULL ||
	    dbus_bus_request_name(service.connection, OBEX_NAME, 0, &error) !=
		    DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
	    dbus_bus_request_name(service.connection, "org.bluez", 0, &error) !=
		    DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr,
			"fplinux Bluetooth test service: cannot claim names: %s\n",
			dbus_error_is_set(&error) ? error.message :
						    "unknown error");
		return 2;
	}
	if (!dbus_connection_add_filter(service.connection, handle, &service,
					NULL))
		fail("cannot register D-Bus handler");
	dbus_connection_flush(service.connection);
	publish_ready(argv[2]);
	long long deadline = milliseconds() + 8000;
	while ((service.stop_at == 0 || milliseconds() < service.stop_at) &&
	       milliseconds() < deadline) {
		if (service.agent_owner != NULL && !service.agent_started)
			begin_authorization(&service);
		if (service.agent_call != NULL &&
		    dbus_pending_call_get_completed(service.agent_call))
			finish_authorization(&service);
		if (service.mode == NETWORK_LATE && service.connected &&
		    release_network) {
			const char *key = "Connected";
			service.connected = false;
			release_network = 0;
			emit_property(service.connection, DEVICE_PATH,
				      "org.bluez.Network1", key,
				      DBUS_TYPE_BOOLEAN, &service.connected);
		}
		dbus_connection_read_write_dispatch(service.connection, 20);
	}
	free(service.agent_owner);
	free(service.agent_path);
	return service.stop_at == 0 ? 2 : 0;
}
