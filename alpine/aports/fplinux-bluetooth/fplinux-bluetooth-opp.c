// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "fplinux-bluetooth-common.h"
#include "fplinux-bluetooth-opp.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

#define OBEX_NAME "org.bluez.obex"
#define OBEX_ROOT "/org/bluez/obex"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define TRANSFER "org.bluez.obex.Transfer1"
#define SESSION "org.bluez.obex.Session1"
#define TIMEOUT_MS 120000

struct watch {
	const char *transfer;
	char pending_transfer[PATH_MAX];
	char pending_status[32];
	bool done;
	bool success;
	bool owner_lost;
	char status[32];
};

struct receiver {
	DBusConnection *connection;
	const char *peer;
	int directory_fd;
	int staging_fd;
	char staging[PATH_MAX];
	char filename[NAME_MAX + 1];
	char transfer[PATH_MAX];
	bool authorized;
	bool done;
	bool success;
	bool owner_lost;
	char status[32];
};

static bool equal_peer(const char *left, const char *right)
{
	return strcasecmp(left, right) == 0;
}

static bool valid_filename(const char *name)
{
	return name[0] != '\0' && strcmp(name, ".") != 0 &&
	       strcmp(name, "..") != 0 && strchr(name, '/') == NULL &&
	       strlen(name) <= NAME_MAX;
}

static bool obex_owner(DBusConnection *connection)
{
	DBusError error;
	dbus_error_init(&error);
	dbus_bool_t owned =
		dbus_bus_name_has_owner(connection, OBEX_NAME, &error);
	if (dbus_error_is_set(&error)) {
		fplinux_bluetooth_fail("cannot query %s: %s", OBEX_NAME,
				       error.message);
		dbus_error_free(&error);
		return false;
	}
	if (!owned)
		fplinux_bluetooth_fail(
			"%s is not running on system D-Bus (start fplinux-obexd)",
			OBEX_NAME);
	return owned;
}

static DBusMessage *method(const char *path, const char *interface,
			   const char *member)
{
	return dbus_message_new_method_call(OBEX_NAME, path, interface, member);
}

static bool append_target(DBusMessage *message)
{
	DBusMessageIter root, array, entry, variant;
	const char *key = "Target";
	const char *target = "opp";
	dbus_message_iter_init_append(message, &root);
	if (!dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}",
					      &array) ||
	    !dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
					      NULL, &entry) ||
	    !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
	    !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s",
					      &variant) ||
	    !dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
					    &target) ||
	    !dbus_message_iter_close_container(&entry, &variant) ||
	    !dbus_message_iter_close_container(&array, &entry) ||
	    !dbus_message_iter_close_container(&root, &array))
		return false;
	return true;
}

static bool get_property(DBusConnection *connection, const char *path,
			 const char *interface, const char *property,
			 int expected_type, char *output, size_t output_size)
{
	DBusMessage *message = dbus_message_new_method_call(OBEX_NAME, path,
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
	if (dbus_message_iter_get_arg_type(&variant) != expected_type) {
		dbus_message_unref(reply);
		return false;
	}
	const char *value;
	dbus_message_iter_get_basic(&variant, &value);
	if (snprintf(output, output_size, "%s", value) >= (int)output_size) {
		dbus_message_unref(reply);
		return false;
	}
	dbus_message_unref(reply);
	return true;
}

static bool get_transfer_session(DBusConnection *connection,
				 const char *transfer, char *session,
				 size_t session_size)
{
	return get_property(connection, transfer, TRANSFER, "Session",
			    DBUS_TYPE_OBJECT_PATH, session, session_size);
}

static bool get_transfer_name(DBusConnection *connection, const char *transfer,
			      char *name, size_t name_size)
{
	return get_property(connection, transfer, TRANSFER, "Name",
			    DBUS_TYPE_STRING, name, name_size);
}

static bool get_session_destination(DBusConnection *connection,
				    const char *session, char *peer,
				    size_t peer_size)
{
	return get_property(connection, session, SESSION, "Destination",
			    DBUS_TYPE_STRING, peer, peer_size);
}

static bool properties_status(DBusMessageIter *array, char *status,
			      size_t status_size)
{
	while (dbus_message_iter_get_arg_type(array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		dbus_message_iter_recurse(array, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return false;
		const char *key;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry) ||
		    dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			return false;
		dbus_message_iter_recurse(&entry, &variant);
		if (strcmp(key, "Status") == 0 &&
		    dbus_message_iter_get_arg_type(&variant) ==
			    DBUS_TYPE_STRING) {
			const char *value;
			dbus_message_iter_get_basic(&variant, &value);
			if (snprintf(status, status_size, "%s", value) >=
			    (int)status_size)
				return false;
			return true;
		}
		dbus_message_iter_next(array);
	}
	return false;
}

static bool signal_status(DBusMessage *message, char *status,
			  size_t status_size)
{
	DBusMessageIter iter, changed;
	const char *interface;
	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&iter, &interface);
	if (strcmp(interface, TRANSFER) != 0 ||
	    !dbus_message_iter_next(&iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return false;
	dbus_message_iter_recurse(&iter, &changed);
	return properties_status(&changed, status, status_size);
}

static void update_status(bool *done, bool *success, char *destination,
			  size_t destination_size, const char *status)
{
	if (snprintf(destination, destination_size, "%s", status) >=
	    (int)destination_size)
		return;
	if (strcmp(status, "complete") == 0) {
		*done = true;
		*success = true;
	} else if (strcmp(status, "error") == 0) {
		*done = true;
		*success = false;
	}
}

static DBusHandlerResult watch_filter(DBusConnection *connection,
				      DBusMessage *message, void *data)
{
	(void)connection;
	struct watch *watch = data;
	if (dbus_message_is_signal(message, PROPERTIES, "PropertiesChanged") &&
	    dbus_message_get_path(message) != NULL) {
		char status[sizeof(watch->status)];
		if (signal_status(message, status, sizeof(status))) {
			if (watch->transfer != NULL &&
			    strcmp(dbus_message_get_path(message),
				   watch->transfer) == 0)
				update_status(&watch->done, &watch->success,
					      watch->status,
					      sizeof(watch->status), status);
			else if (watch->transfer == NULL &&
				 snprintf(watch->pending_transfer,
					  sizeof(watch->pending_transfer), "%s",
					  dbus_message_get_path(message)) <
					 (int)sizeof(watch->pending_transfer))
				snprintf(watch->pending_status,
					 sizeof(watch->pending_status), "%s",
					 status);
		}
	} else if (dbus_message_is_signal(message, "org.freedesktop.DBus",
					  "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;
		if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING,
					  &name, DBUS_TYPE_STRING, &old_owner,
					  DBUS_TYPE_STRING, &new_owner,
					  DBUS_TYPE_INVALID) &&
		    strcmp(name, OBEX_NAME) == 0 && old_owner[0] != '\0' &&
		    new_owner[0] == '\0')
			watch->owner_lost = true;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void add_matches(DBusConnection *connection, const char *path)
{
	DBusError error;
	char rule[PATH_MAX + 160];
	dbus_error_init(&error);
	if (path != NULL) {
		snprintf(
			rule, sizeof(rule),
			"type='signal',interface='%s',member='PropertiesChanged',path='%s'",
			PROPERTIES, path);
		dbus_bus_add_match(connection, rule, &error);
	} else {
		dbus_bus_add_match(
			connection,
			"type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.obex.Transfer1'",
			&error);
	}
	dbus_bus_add_match(
		connection,
		"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez.obex'",
		&error);
	if (dbus_error_is_set(&error)) {
		fplinux_bluetooth_fail("cannot subscribe to D-Bus signals: %s",
				       error.message);
		dbus_error_free(&error);
	}
	dbus_connection_flush(connection);
}

static int wait_for_transfer(DBusConnection *connection, struct watch *watch,
			     int seconds,
			     const volatile sig_atomic_t *interrupted)
{
	int64_t deadline = fplinux_bluetooth_now_ms() + (int64_t)seconds * 1000;
	while (!*interrupted && !watch->done && !watch->owner_lost) {
		int64_t remaining = deadline - fplinux_bluetooth_now_ms();
		if (remaining <= 0)
			break;
		if (!dbus_connection_read_write_dispatch(
			    connection,
			    remaining > 1000 ? 1000 : (int)remaining))
			return fplinux_bluetooth_fail(
				"system D-Bus disconnected");
	}
	if (*interrupted)
		return fplinux_bluetooth_fail("interrupted");
	if (watch->owner_lost)
		return fplinux_bluetooth_fail(
			"%s stopped while transfer was active", OBEX_NAME);
	if (!watch->done)
		return fplinux_bluetooth_fail(
			"transfer timed out after %d seconds", seconds);
	if (!watch->success)
		return fplinux_bluetooth_fail("transfer ended with status %s",
					      watch->status[0] ? watch->status :
								 "error");
	return 0;
}

static void remove_session(DBusConnection *connection, const char *session)
{
	DBusMessage *message =
		method(OBEX_ROOT, "org.bluez.obex.Client1", "RemoveSession");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &session,
				      DBUS_TYPE_INVALID)) {
		if (message != NULL)
			dbus_message_unref(message);
		return;
	}
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply != NULL)
		dbus_message_unref(reply);
}

int fplinux_bluetooth_opp_send(const char *peer, const char *file,
			       const volatile sig_atomic_t *interrupted)
{
	struct stat information;
	if (stat(file, &information) < 0 || !S_ISREG(information.st_mode))
		return fplinux_bluetooth_fail(
			"%s is not a regular readable file", file);
	DBusConnection *connection = fplinux_bluetooth_system_bus();
	if (connection == NULL || !obex_owner(connection))
		return 1;
	DBusMessage *message =
		method(OBEX_ROOT, "org.bluez.obex.Client1", "CreateSession");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &peer,
				      DBUS_TYPE_INVALID) ||
	    !append_target(message))
		return fplinux_bluetooth_fail(
			"cannot form CreateSession request");
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply == NULL)
		return 1;
	const char *session;
	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &session,
				   DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		return fplinux_bluetooth_fail(
			"CreateSession returned an invalid reply");
	}
	char session_copy[PATH_MAX];
	if (snprintf(session_copy, sizeof(session_copy), "%s", session) >=
	    (int)sizeof(session_copy)) {
		dbus_message_unref(reply);
		return fplinux_bluetooth_fail(
			"CreateSession returned an overlong object path");
	}
	dbus_message_unref(reply);
	message =
		method(session_copy, "org.bluez.obex.ObjectPush1", "SendFile");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &file,
				      DBUS_TYPE_INVALID)) {
		remove_session(connection, session_copy);
		return fplinux_bluetooth_fail("cannot form SendFile request");
	}
	/* Subscribe before SendFile: BlueZ may publish and remove a fast transfer
	 * before its method reply reaches us.  A terminal signal dispatched before
	 * its returned object path is retained until that path is known. */
	struct watch watch = { 0 };
	add_matches(connection, NULL);
	if (!dbus_connection_add_filter(connection, watch_filter, &watch,
					NULL)) {
		dbus_message_unref(message);
		remove_session(connection, session_copy);
		return fplinux_bluetooth_fail(
			"cannot install D-Bus transfer monitor");
	}
	reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply == NULL) {
		dbus_connection_remove_filter(connection, watch_filter, &watch);
		remove_session(connection, session_copy);
		return 1;
	}
	DBusMessageIter iter;
	if (!dbus_message_iter_init(reply, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
		dbus_message_unref(reply);
		dbus_connection_remove_filter(connection, watch_filter, &watch);
		remove_session(connection, session_copy);
		return fplinux_bluetooth_fail(
			"SendFile returned an invalid reply");
	}
	const char *transfer;
	dbus_message_iter_get_basic(&iter, &transfer);
	char transfer_copy[PATH_MAX];
	if (snprintf(transfer_copy, sizeof(transfer_copy), "%s", transfer) >=
	    (int)sizeof(transfer_copy)) {
		dbus_message_unref(reply);
		dbus_connection_remove_filter(connection, watch_filter, &watch);
		remove_session(connection, session_copy);
		return fplinux_bluetooth_fail(
			"SendFile returned an overlong object path");
	}
	watch.transfer = transfer_copy;
	if (strcmp(watch.pending_transfer, transfer_copy) == 0 &&
	    watch.pending_status[0] != '\0')
		update_status(&watch.done, &watch.success, watch.status,
			      sizeof(watch.status), watch.pending_status);
	if (dbus_message_iter_next(&iter) &&
	    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		DBusMessageIter properties;
		dbus_message_iter_recurse(&iter, &properties);
		char returned[sizeof(watch.status)];
		if (properties_status(&properties, returned, sizeof(returned)))
			update_status(&watch.done, &watch.success, watch.status,
				      sizeof(watch.status), returned);
	}
	dbus_message_unref(reply);
	char initial[sizeof(watch.status)];
	if (get_property(connection, transfer_copy, TRANSFER, "Status",
			 DBUS_TYPE_STRING, initial, sizeof(initial)))
		update_status(&watch.done, &watch.success, watch.status,
			      sizeof(watch.status), initial);
	int result = wait_for_transfer(connection, &watch, TIMEOUT_MS / 1000,
				       interrupted);
	dbus_connection_remove_filter(connection, watch_filter, &watch);
	remove_session(connection, session_copy);
	if (result == 0)
		printf("sent %s to %s\n", file, peer);
	return result;
}

static DBusHandlerResult receiver_filter(DBusConnection *connection,
					 DBusMessage *message, void *data)
{
	struct receiver *receiver = data;
	struct watch watch = { .transfer = receiver->transfer };
	watch_filter(connection, message, &watch);
	if (watch.owner_lost)
		receiver->owner_lost = true;
	if (watch.done) {
		receiver->done = true;
		receiver->success = watch.success;
		snprintf(receiver->status, sizeof(receiver->status), "%s",
			 watch.status);
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusMessage *rejected(DBusMessage *message, const char *reason)
{
	return dbus_message_new_error(message, "org.bluez.obex.Error.Rejected",
				      reason);
}

static DBusHandlerResult agent_message(DBusConnection *connection,
				       DBusMessage *message, void *data)
{
	struct receiver *receiver = data;
	if (!dbus_message_has_interface(message, "org.bluez.obex.Agent1"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (dbus_message_is_method_call(message, "org.bluez.obex.Agent1",
					"Release") ||
	    dbus_message_is_method_call(message, "org.bluez.obex.Agent1",
					"Cancel")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		if (reply != NULL) {
			dbus_connection_send(connection, reply, NULL);
			dbus_connection_flush(connection);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!dbus_message_is_method_call(message, "org.bluez.obex.Agent1",
					 "AuthorizePush"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *transfer;
	if (receiver->authorized ||
	    !dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH,
				   &transfer, DBUS_TYPE_INVALID))
		goto reject;
	char session[PATH_MAX], sender[32], suggested[PATH_MAX];
	if (!get_transfer_session(connection, transfer, session,
				  sizeof(session)) ||
	    !get_session_destination(connection, session, sender,
				     sizeof(sender)) ||
	    !equal_peer(sender, receiver->peer) ||
	    !get_transfer_name(connection, transfer, suggested,
			       sizeof(suggested)))
		goto reject;
	const char *name = strrchr(suggested, '/');
	name = name == NULL ? suggested : name + 1;
	if (!valid_filename(name) ||
	    snprintf(receiver->filename, sizeof(receiver->filename), "%s",
		     name) >= (int)sizeof(receiver->filename))
		goto reject;
	char target[PATH_MAX];
	if (snprintf(target, sizeof(target), "%s/%s", receiver->staging,
		     receiver->filename) >= (int)sizeof(target))
		goto reject;
	if (snprintf(receiver->transfer, sizeof(receiver->transfer), "%s",
		     transfer) >= (int)sizeof(receiver->transfer))
		goto reject;
	DBusMessage *reply = dbus_message_new_method_return(message);
	const char *target_path = target;
	if (reply == NULL ||
	    !dbus_message_append_args(reply, DBUS_TYPE_STRING, &target_path,
				      DBUS_TYPE_INVALID)) {
		if (reply != NULL)
			dbus_message_unref(reply);
		goto reject;
	}
	receiver->authorized = true;
	dbus_connection_send(connection, reply, NULL);
	dbus_connection_flush(connection);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
reject: {
	DBusMessage *reply = rejected(
		message, "push is not permitted by this receive window");
	if (reply != NULL) {
		dbus_connection_send(connection, reply, NULL);
		dbus_connection_flush(connection);
		dbus_message_unref(reply);
	}
}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int move_received(struct receiver *receiver)
{
#ifdef SYS_renameat2
	long result = syscall(SYS_renameat2, receiver->staging_fd,
			      receiver->filename, receiver->directory_fd,
			      receiver->filename, RENAME_NOREPLACE);
	if (result == 0)
		return 0;
	if (errno == EEXIST)
		return fplinux_bluetooth_fail("refusing to overwrite %s",
					      receiver->filename);
	if (errno == ENOSYS)
		return fplinux_bluetooth_fail(
			"kernel lacks renameat2(RENAME_NOREPLACE); received file remains in %s",
			receiver->staging);
	return fplinux_bluetooth_fail(
		"cannot place received file without overwrite: %s",
		strerror(errno));
#else
	return fplinux_bluetooth_fail(
		"build lacks renameat2(RENAME_NOREPLACE); received file remains in %s",
		receiver->staging);
#endif
}

static void unregister_agent(DBusConnection *connection, const char *path)
{
	DBusMessage *message = method(OBEX_ROOT, "org.bluez.obex.AgentManager1",
				      "UnregisterAgent");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_INVALID)) {
		if (message != NULL)
			dbus_message_unref(message);
		return;
	}
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply != NULL)
		dbus_message_unref(reply);
}

int fplinux_bluetooth_opp_receive(const char *peer, const char *directory,
				  int seconds,
				  const volatile sig_atomic_t *interrupted)
{
	char resolved[PATH_MAX];
	if (realpath(directory, resolved) == NULL)
		return fplinux_bluetooth_fail("cannot resolve %s: %s",
					      directory, strerror(errno));
	struct receiver receiver = { .peer = peer,
				     .directory_fd = -1,
				     .staging_fd = -1 };
	receiver.directory_fd =
		open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (receiver.directory_fd < 0)
		return fplinux_bluetooth_fail(
			"cannot open receive directory %s: %s", resolved,
			strerror(errno));
	if (snprintf(receiver.staging, sizeof(receiver.staging),
		     "%s/.fplinux-bluetooth-XXXXXX",
		     resolved) >= (int)sizeof(receiver.staging))
		return fplinux_bluetooth_fail(
			"receive directory path is too long");
	if (mkdtemp(receiver.staging) == NULL)
		return fplinux_bluetooth_fail(
			"cannot create private receive staging directory: %s",
			strerror(errno));
	receiver.staging_fd =
		open(receiver.staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (receiver.staging_fd < 0)
		return fplinux_bluetooth_fail(
			"cannot open receive staging directory: %s",
			strerror(errno));
	DBusConnection *connection = fplinux_bluetooth_system_bus();
	if (connection == NULL || !obex_owner(connection))
		return 1;
	receiver.connection = connection;
	static const DBusObjectPathVTable vtable = { .message_function =
							     agent_message };
	const char *agent_path = "/org/fplinux/bluetooth/agent";
	if (!dbus_connection_register_object_path(connection, agent_path,
						  &vtable, &receiver))
		return fplinux_bluetooth_fail(
			"cannot register receive authorization agent");
	DBusMessage *message = method(OBEX_ROOT, "org.bluez.obex.AgentManager1",
				      "RegisterAgent");
	if (message == NULL ||
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH,
				      &agent_path, DBUS_TYPE_INVALID))
		return fplinux_bluetooth_fail(
			"cannot form RegisterAgent request");
	DBusMessage *reply = fplinux_bluetooth_call(connection, message, 15000);
	if (reply == NULL)
		return 1;
	dbus_message_unref(reply);
	if (!dbus_connection_add_filter(connection, receiver_filter, &receiver,
					NULL))
		return fplinux_bluetooth_fail("cannot install receive monitor");
	add_matches(connection, NULL);
	printf("accepting one OPP file from %s for %d seconds\n", peer,
	       seconds);
	int64_t deadline = fplinux_bluetooth_now_ms() + (int64_t)seconds * 1000;
	while (!*interrupted && !receiver.done && !receiver.owner_lost) {
		int64_t remaining = deadline - fplinux_bluetooth_now_ms();
		if (remaining <= 0)
			break;
		if (!dbus_connection_read_write_dispatch(
			    connection,
			    remaining > 1000 ? 1000 : (int)remaining)) {
			fplinux_bluetooth_fail("system D-Bus disconnected");
			break;
		}
	}
	dbus_connection_remove_filter(connection, receiver_filter, &receiver);
	unregister_agent(connection, agent_path);
	dbus_connection_unregister_object_path(connection, agent_path);
	int result;
	if (*interrupted)
		result = fplinux_bluetooth_fail("interrupted");
	else if (receiver.owner_lost)
		result = fplinux_bluetooth_fail("%s stopped while receiving",
						OBEX_NAME);
	else if (!receiver.done)
		result = fplinux_bluetooth_fail("receive window expired");
	else if (!receiver.success)
		result = fplinux_bluetooth_fail(
			"received transfer ended with status %s",
			receiver.status[0] ? receiver.status : "error");
	else {
		result = move_received(&receiver);
		if (result == 0)
			printf("received %s/%s\n", resolved, receiver.filename);
	}
	if (receiver.staging_fd >= 0)
		close(receiver.staging_fd);
	if (receiver.directory_fd >= 0)
		close(receiver.directory_fd);
	if (result == 0)
		rmdir(receiver.staging);
	else
		fprintf(stderr, "fplinux-bluetooth: staging retained at %s\n",
			receiver.staging);
	return result;
}
