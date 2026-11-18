// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
#define AGENT "org.bluez.obex.Agent1"
#define SEND_TIMEOUT_SECONDS 120

struct operation {
	DBusConnection *connection;
	const struct fplinux_bluetooth_opp_callbacks *callbacks;
	struct fplinux_bluetooth_opp_result *result;
	struct fplinux_bluetooth_opp_progress progress;
	const char *session;
	char owner[256];
	char transfer[PATH_MAX];
	char pending_transfer[PATH_MAX];
	char status[32];
	char pending_status[32];
	uint64_t pending_bytes;
	bool pending_bytes_known;
	bool done;
	bool success;
	bool owner_lost;
	bool monitor_installed;
	void (*poll)(struct operation *operation);
};

struct receiver {
	struct operation operation;
	int directory_fd;
	int staging_fd;
	int file_fd;
	char directory[PATH_MAX];
	char staging[PATH_MAX];
	char filename[NAME_MAX + 1];
	bool authorized;
	bool agent_registered;
	bool object_registered;
	bool offer_ready;
	DBusMessage *authorization;
	char offered_transfer[PATH_MAX];
};

static bool is_cancelled(void *data)
{
	struct operation *operation = data;
	return operation->callbacks != NULL &&
	       operation->callbacks->cancelled != NULL &&
	       operation->callbacks->cancelled(operation->callbacks->data);
}

static bool call_cancelled(void *data)
{
	struct operation *operation = data;
	return is_cancelled(operation) || operation->owner_lost;
}

static int fail(struct operation *operation, int error_number,
		const char *format, ...)
{
	va_list arguments;
	struct fplinux_bluetooth_opp_result *result = operation->result;

	result->error_number = error_number;
	result->outcome = error_number == ECANCELED ?
				  FPLINUX_BLUETOOTH_OPP_CANCELLED :
				  FPLINUX_BLUETOOTH_OPP_FAILED;
	va_start(arguments, format);
	vsnprintf(result->message, sizeof(result->message), format, arguments);
	va_end(arguments);
	return 1;
}

static void report(struct operation *operation,
		   enum fplinux_bluetooth_opp_state state)
{
	operation->progress.state = state;
	if (operation->callbacks != NULL &&
	    operation->callbacks->progress != NULL)
		operation->callbacks->progress(&operation->progress,
					       operation->callbacks->data);
}

static DBusMessage *method(const char *path, const char *interface,
			   const char *member)
{
	return dbus_message_new_method_call(OBEX_NAME, path, interface, member);
}

static DBusMessage *call(struct operation *operation, DBusMessage *message,
			 bool optional)
{
	DBusError error;
	dbus_error_init(&error);
	if (message == NULL) {
		if (!optional)
			fail(operation, ENOMEM, "cannot form D-Bus request");
		return NULL;
	}
	if (operation->owner[0] != '\0' &&
	    dbus_message_has_destination(message, OBEX_NAME) &&
	    !dbus_message_set_destination(message, operation->owner)) {
		dbus_message_unref(message);
		if (!optional)
			fail(operation, ENOMEM, "cannot address D-Bus request");
		return NULL;
	}
	DBusMessage *reply = fplinux_bluetooth_call_cancellable(
		operation->connection, message, 15000, call_cancelled,
		operation, &error);
	if (reply == NULL && !optional) {
		if (is_cancelled(operation))
			fail(operation, ECANCELED, "interrupted");
		else if (operation->owner_lost)
			fail(operation, ENOTCONN,
			     "%s stopped while transfer was active", OBEX_NAME);
		else
			fail(operation, EIO, "D-Bus call failed: %s",
			     error.message ?: "unknown error");
	}
	dbus_error_free(&error);
	return reply;
}

static bool valid_filename(const char *name)
{
	return name[0] != '\0' && strcmp(name, ".") != 0 &&
	       strcmp(name, "..") != 0 && strchr(name, '/') == NULL &&
	       strlen(name) <= NAME_MAX;
}

static bool join_path(char *output, size_t capacity, const char *directory,
		      const char *filename)
{
	size_t directory_size = strlen(directory);
	size_t filename_size = strlen(filename);
	if (directory_size >= capacity ||
	    filename_size >= capacity - directory_size - 1)
		return false;
	memcpy(output, directory, directory_size);
	output[directory_size] = '/';
	memcpy(output + directory_size + 1, filename, filename_size + 1);
	return true;
}

static DBusMessage *property(struct operation *operation, const char *path,
			     const char *interface, const char *name,
			     bool optional)
{
	DBusMessage *message = method(path, PROPERTIES, "Get");
	if (message != NULL &&
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &interface,
				      DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	return call(operation, message, optional);
}

static bool property_value(DBusMessage *reply, int type, void *value)
{
	DBusMessageIter iter, variant;
	if (!dbus_message_iter_init(reply, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return false;
	dbus_message_iter_recurse(&iter, &variant);
	if (dbus_message_iter_get_arg_type(&variant) != type)
		return false;
	dbus_message_iter_get_basic(&variant, value);
	return true;
}

static bool get_text(struct operation *operation, const char *path,
		     const char *interface, const char *name, int type,
		     char *output, size_t size, bool optional)
{
	DBusMessage *reply =
		property(operation, path, interface, name, optional);
	if (reply == NULL)
		return false;
	const char *text;
	bool valid = property_value(reply, type, &text);
	if (valid)
		valid = snprintf(output, size, "%s", text) < (int)size;
	dbus_message_unref(reply);
	return valid;
}

static void get_size(struct operation *operation, const char *transfer)
{
	DBusMessage *reply =
		property(operation, transfer, TRANSFER, "Size", true);
	if (reply == NULL)
		return;
	uint64_t size;
	if (property_value(reply, DBUS_TYPE_UINT64, &size)) {
		operation->progress.total_bytes = size;
		operation->progress.total_known = true;
	}
	dbus_message_unref(reply);
}

static void apply_status(struct operation *operation, const char *status)
{
	snprintf(operation->status, sizeof(operation->status), "%s", status);
	if (strcmp(status, "complete") == 0) {
		operation->done = true;
		operation->success = true;
		if (operation->progress.total_known &&
		    operation->progress.transferred_bytes <
			    operation->progress.total_bytes)
			operation->progress.transferred_bytes =
				operation->progress.total_bytes;
	} else if (strcmp(status, "error") == 0) {
		operation->done = true;
		operation->success = false;
	}
}

static void read_properties(struct operation *operation, DBusMessageIter *array,
			    bool pending)
{
	while (dbus_message_iter_get_arg_type(array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *key;
		dbus_message_iter_recurse(array, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry) ||
		    dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			return;
		dbus_message_iter_recurse(&entry, &variant);
		int type = dbus_message_iter_get_arg_type(&variant);
		if (strcmp(key, "Status") == 0 && type == DBUS_TYPE_STRING) {
			const char *text;
			dbus_message_iter_get_basic(&variant, &text);
			if (pending)
				snprintf(operation->pending_status,
					 sizeof(operation->pending_status),
					 "%s", text);
			else
				apply_status(operation, text);
		} else if (strcmp(key, "Transferred") == 0 &&
			   type == DBUS_TYPE_UINT64) {
			uint64_t bytes;
			dbus_message_iter_get_basic(&variant, &bytes);
			if (pending) {
				operation->pending_bytes = bytes;
				operation->pending_bytes_known = true;
			} else if (bytes >
				   operation->progress.transferred_bytes) {
				operation->progress.transferred_bytes = bytes;
			}
		} else if (!pending && strcmp(key, "Size") == 0 &&
			   type == DBUS_TYPE_UINT64) {
			dbus_message_iter_get_basic(
				&variant, &operation->progress.total_bytes);
			operation->progress.total_known = true;
		}
		dbus_message_iter_next(array);
	}
}

static DBusHandlerResult monitor(DBusConnection *connection,
				 DBusMessage *message, void *data)
{
	(void)connection;
	struct operation *operation = data;
	if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS,
				   "NameOwnerChanged") &&
	    dbus_message_has_sender(message, DBUS_SERVICE_DBUS)) {
		const char *name, *old_owner, *new_owner;
		if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING,
					  &name, DBUS_TYPE_STRING, &old_owner,
					  DBUS_TYPE_STRING, &new_owner,
					  DBUS_TYPE_INVALID) &&
		    strcmp(name, OBEX_NAME) == 0 &&
		    strcmp(old_owner, operation->owner) == 0 &&
		    strcmp(new_owner, old_owner) != 0)
			operation->owner_lost = true;
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (!dbus_message_has_sender(message, operation->owner) ||
	    !dbus_message_is_signal(message, PROPERTIES, "PropertiesChanged"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *path = dbus_message_get_path(message);
	if (path == NULL || (operation->transfer[0] != '\0' &&
			     strcmp(operation->transfer, path) != 0))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	DBusMessageIter iter, array;
	const char *interface;
	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	dbus_message_iter_get_basic(&iter, &interface);
	if (strcmp(interface, TRANSFER) != 0 ||
	    !dbus_message_iter_next(&iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	bool pending = operation->transfer[0] == '\0';
	if (pending && operation->session != NULL) {
		size_t session_size = strlen(operation->session);
		if (strncmp(path, operation->session, session_size) != 0 ||
		    path[session_size] != '/')
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (pending && strcmp(operation->pending_transfer, path) != 0) {
		snprintf(operation->pending_transfer,
			 sizeof(operation->pending_transfer), "%s", path);
		operation->pending_status[0] = '\0';
		operation->pending_bytes_known = false;
	}
	dbus_message_iter_recurse(&iter, &array);
	read_properties(operation, &array, pending);
	if (!pending)
		report(operation, FPLINUX_BLUETOOTH_OPP_TRANSFERRING);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool open_bus(struct operation *operation)
{
	DBusError error;
	dbus_error_init(&error);
	operation->connection = fplinux_bluetooth_private_bus(&error);
	if (operation->connection == NULL) {
		fail(operation, EIO, "cannot connect to system D-Bus: %s",
		     error.message ?: "unknown error");
		dbus_error_free(&error);
		return false;
	}
	DBusMessage *message = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
							    DBUS_PATH_DBUS,
							    DBUS_INTERFACE_DBUS,
							    "GetNameOwner");
	const char *name = OBEX_NAME;
	if (message != NULL &&
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	DBusMessage *reply = call(operation, message, false);
	if (reply == NULL)
		return false;
	const char *owner;
	bool valid = dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING,
					   &owner, DBUS_TYPE_INVALID);
	if (valid)
		valid = snprintf(operation->owner, sizeof(operation->owner),
				 "%s", owner) < (int)sizeof(operation->owner);
	dbus_message_unref(reply);
	if (!valid) {
		fail(operation, EPROTO, "invalid OBEX owner reply");
		return false;
	}
	static const char *rules[] = {
		"type='signal',sender='org.bluez.obex',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.obex.Transfer1'",
		"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez.obex'",
	};
	for (size_t index = 0; index < sizeof(rules) / sizeof(rules[0]);
	     ++index) {
		message = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
						       DBUS_PATH_DBUS,
						       DBUS_INTERFACE_DBUS,
						       "AddMatch");
		if (message != NULL &&
		    !dbus_message_append_args(message, DBUS_TYPE_STRING,
					      &rules[index],
					      DBUS_TYPE_INVALID)) {
			dbus_message_unref(message);
			message = NULL;
		}
		reply = call(operation, message, false);
		if (reply == NULL)
			return false;
		dbus_message_unref(reply);
	}
	if (!dbus_connection_add_filter(operation->connection, monitor,
					operation, NULL)) {
		fail(operation, ENOMEM,
		     "cannot install D-Bus transfer monitor");
		return false;
	}
	operation->monitor_installed = true;
	return true;
}

static void close_bus(struct operation *operation)
{
	if (operation->monitor_installed)
		dbus_connection_remove_filter(operation->connection, monitor,
					      operation);
	fplinux_bluetooth_close_bus(operation->connection);
}

static void cleanup_call(struct operation *operation, DBusMessage *message)
{
	if (message == NULL)
		return;
	if (operation->connection == NULL) {
		dbus_message_unref(message);
		return;
	}
	if (operation->owner[0] != '\0' &&
	    dbus_message_has_destination(message, OBEX_NAME) &&
	    !dbus_message_set_destination(message, operation->owner)) {
		dbus_message_unref(message);
		return;
	}
	DBusError error;
	dbus_error_init(&error);
	/* Cleanup is bounded even after the caller has requested cancellation. */
	DBusMessage *reply = fplinux_bluetooth_call_cancellable(
		operation->connection, message, 200, NULL, NULL, &error);
	if (reply != NULL)
		dbus_message_unref(reply);
	dbus_error_free(&error);
}

static void cancel_transfer(struct operation *operation)
{
	if (operation->transfer[0] != '\0' && !operation->done)
		cleanup_call(operation,
			     method(operation->transfer, TRANSFER, "Cancel"));
}

static void session_method(struct operation *operation, const char *member,
			   const char *session)
{
	DBusMessage *message =
		method(OBEX_ROOT, "org.bluez.obex.Client1", member);
	if (message != NULL &&
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &session,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		return;
	}
	cleanup_call(operation, message);
}

static int wait_for_transfer(struct operation *operation, int seconds,
			     bool receiving)
{
	int64_t deadline = fplinux_bluetooth_now_ms() + (int64_t)seconds * 1000;
	while (!is_cancelled(operation) && !operation->done &&
	       !operation->owner_lost) {
		if (operation->poll != NULL)
			operation->poll(operation);
		if (is_cancelled(operation) || operation->done ||
		    operation->owner_lost)
			break;
		int64_t remaining = deadline - fplinux_bluetooth_now_ms();
		if (remaining <= 0)
			break;
		if (!dbus_connection_read_write_dispatch(
			    operation->connection,
			    remaining > 50 ? 50 : (int)remaining))
			return fail(operation, ENOTCONN,
				    "system D-Bus disconnected");
	}
	if (is_cancelled(operation))
		return fail(operation, ECANCELED, "interrupted");
	if (operation->done && operation->success)
		return 0;
	if (operation->owner_lost)
		return fail(operation, ENOTCONN,
			    "%s stopped while transfer was active", OBEX_NAME);
	if (!operation->done)
		return receiving ? fail(operation, ETIMEDOUT,
					"receive window expired") :
				   fail(operation, ETIMEDOUT,
					"transfer timed out after %d seconds",
					seconds);
	if (!operation->success && operation->result->message[0] != '\0')
		return 1;
	if (!operation->success)
		return fail(operation, EIO, "transfer ended with status %s",
			    operation->status[0] ? operation->status : "error");
	return 0;
}

static bool append_target(DBusMessage *message)
{
	DBusMessageIter root, array, entry, variant;
	const char *key = "Target", *target = "opp";
	dbus_message_iter_init_append(message, &root);
	return dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}",
						&array) &&
	       dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry) &&
	       dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
	       dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s",
						&variant) &&
	       dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
					      &target) &&
	       dbus_message_iter_close_container(&entry, &variant) &&
	       dbus_message_iter_close_container(&array, &entry) &&
	       dbus_message_iter_close_container(&root, &array);
}

int fplinux_bluetooth_opp_send(
	const char *peer, const char *file,
	const struct fplinux_bluetooth_opp_callbacks *callbacks,
	struct fplinux_bluetooth_opp_result *result)
{
	*result = (struct fplinux_bluetooth_opp_result){ 0 };
	struct operation operation = {
		.callbacks = callbacks,
		.result = result,
		.progress = { .peer = peer, .filename = file },
	};
	struct stat information;
	char resolved[PATH_MAX], session[PATH_MAX] = "";
	int status = 1;
	if (realpath(file, resolved) == NULL ||
	    stat(resolved, &information) < 0 || !S_ISREG(information.st_mode))
		return fail(&operation, EINVAL,
			    "%s is not a regular readable file", file);
	operation.progress.total_bytes = (uint64_t)information.st_size;
	operation.progress.total_known = true;
	report(&operation, FPLINUX_BLUETOOTH_OPP_CONNECTING);
	if (!open_bus(&operation))
		goto out;
	DBusMessage *message =
		method(OBEX_ROOT, "org.bluez.obex.Client1", "CreateSession");
	if (message != NULL &&
	    (!dbus_message_append_args(message, DBUS_TYPE_STRING, &peer,
				       DBUS_TYPE_INVALID) ||
	     !append_target(message))) {
		dbus_message_unref(message);
		message = NULL;
	}
	DBusMessage *reply = call(&operation, message, false);
	if (reply == NULL)
		goto out;
	const char *path;
	bool valid = dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH,
					   &path, DBUS_TYPE_INVALID);
	if (valid)
		valid = snprintf(session, sizeof(session), "%s", path) <
			(int)sizeof(session);
	dbus_message_unref(reply);
	if (!valid) {
		fail(&operation, EPROTO,
		     "CreateSession returned an invalid reply");
		goto out;
	}
	operation.session = session;
	message = method(session, "org.bluez.obex.ObjectPush1", "SendFile");
	const char *source = resolved;
	if (message != NULL &&
	    !dbus_message_append_args(message, DBUS_TYPE_STRING, &source,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	report(&operation, FPLINUX_BLUETOOTH_OPP_TRANSFERRING);
	reply = call(&operation, message, false);
	if (reply == NULL)
		goto out;
	DBusMessageIter iter;
	valid = dbus_message_iter_init(reply, &iter) &&
		dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH;
	if (valid) {
		dbus_message_iter_get_basic(&iter, &path);
		valid = snprintf(operation.transfer, sizeof(operation.transfer),
				 "%s", path) < (int)sizeof(operation.transfer);
	}
	if (valid && dbus_message_iter_next(&iter) &&
	    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		DBusMessageIter array;
		dbus_message_iter_recurse(&iter, &array);
		read_properties(&operation, &array, false);
	}
	dbus_message_unref(reply);
	if (!valid) {
		fail(&operation, EPROTO, "SendFile returned an invalid reply");
		goto out;
	}
	/* Terminal signals may precede the reply and object removal. */
	if (strcmp(operation.pending_transfer, operation.transfer) == 0) {
		if (operation.pending_status[0] != '\0')
			apply_status(&operation, operation.pending_status);
		if (operation.pending_bytes_known &&
		    operation.pending_bytes >
			    operation.progress.transferred_bytes)
			operation.progress.transferred_bytes =
				operation.pending_bytes;
	}
	char initial[32];
	if (!operation.done &&
	    get_text(&operation, operation.transfer, TRANSFER, "Status",
		     DBUS_TYPE_STRING, initial, sizeof(initial), true))
		apply_status(&operation, initial);
	status = wait_for_transfer(&operation, SEND_TIMEOUT_SECONDS, false);
	if (status == 0) {
		result->outcome = FPLINUX_BLUETOOTH_OPP_SUCCEEDED;
		report(&operation, FPLINUX_BLUETOOTH_OPP_COMPLETE);
	}
out:
	if (status != 0)
		cancel_transfer(&operation);
	if (session[0] != '\0')
		session_method(&operation, "RemoveSession", session);
	close_bus(&operation);
	return status;
}

static void agent_reply(DBusConnection *connection, DBusMessage *reply)
{
	if (reply == NULL)
		return;
	dbus_connection_send(connection, reply, NULL);
	dbus_connection_flush(connection);
	dbus_message_unref(reply);
}

static DBusHandlerResult agent_message(DBusConnection *connection,
				       DBusMessage *message, void *data)
{
	struct receiver *receiver = data;
	struct operation *operation = &receiver->operation;
	if (!dbus_message_has_interface(message, AGENT) ||
	    !dbus_message_has_sender(message, operation->owner))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (dbus_message_is_method_call(message, AGENT, "Release") ||
	    dbus_message_is_method_call(message, AGENT, "Cancel")) {
		operation->done = true;
		operation->success = false;
		apply_status(operation, "error");
		agent_reply(connection,
			    dbus_message_new_method_return(message));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!dbus_message_is_method_call(message, AGENT, "AuthorizePush"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *transfer;
	if (receiver->authorized || receiver->authorization != NULL ||
	    is_cancelled(operation) ||
	    !dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH,
				   &transfer, DBUS_TYPE_INVALID))
		goto reject;
	if (snprintf(receiver->offered_transfer,
		     sizeof(receiver->offered_transfer), "%s",
		     transfer) >= (int)sizeof(receiver->offered_transfer))
		goto reject;
	/* Property requests must run outside the object-path dispatcher. */
	receiver->authorization = dbus_message_ref(message);
	receiver->offer_ready = false;
	return DBUS_HANDLER_RESULT_HANDLED;
reject:
	agent_reply(connection,
		    dbus_message_new_error(
			    message, "org.bluez.obex.Error.Rejected",
			    "push is not permitted by this receive window"));
	return DBUS_HANDLER_RESULT_HANDLED;
}

static bool prepare_offer(struct receiver *receiver)
{
	struct operation *operation = &receiver->operation;
	const char *transfer = receiver->offered_transfer;
	char session[PATH_MAX], sender[32], suggested[PATH_MAX];
	if (!get_text(operation, transfer, TRANSFER, "Session",
		      DBUS_TYPE_OBJECT_PATH, session, sizeof(session), true) ||
	    !get_text(operation, session, SESSION, "Destination",
		      DBUS_TYPE_STRING, sender, sizeof(sender), true) ||
	    strcasecmp(sender, operation->progress.peer) != 0 ||
	    !get_text(operation, transfer, TRANSFER, "Name", DBUS_TYPE_STRING,
		      suggested, sizeof(suggested), true))
		return false;
	const char *name = strrchr(suggested, '/');
	name = name == NULL ? suggested : name + 1;
	if (!valid_filename(name))
		return false;
	memcpy(receiver->filename, name, strlen(name) + 1);
	operation->progress.filename = receiver->filename;
	operation->progress.total_bytes = 0;
	operation->progress.total_known = false;
	operation->progress.transferred_bytes = 0;
	get_size(operation, transfer);
	if (is_cancelled(operation) || operation->done || operation->owner_lost)
		return false;
	receiver->offer_ready = true;
	report(operation, FPLINUX_BLUETOOTH_OPP_AUTHORIZING);
	return true;
}

static void poll_authorization(struct operation *operation)
{
	struct receiver *receiver = (struct receiver *)operation;
	DBusMessage *message = receiver->authorization;
	if (message == NULL)
		return;
	if (is_cancelled(operation) || operation->done || operation->owner_lost)
		goto reject;
	if (!receiver->offer_ready && !prepare_offer(receiver))
		goto reject;
	struct fplinux_bluetooth_opp_offer offer = {
		.peer = operation->progress.peer,
		.filename = receiver->filename,
		.total_bytes = operation->progress.total_bytes,
		.total_known = operation->progress.total_known,
	};
	enum fplinux_bluetooth_opp_consent consent =
		operation->callbacks->authorize(&offer,
						operation->callbacks->data);
	if (consent == FPLINUX_BLUETOOTH_OPP_PENDING)
		return;
	if (consent != FPLINUX_BLUETOOTH_OPP_ACCEPT || is_cancelled(operation))
		goto reject;
	struct statvfs space;
	if (operation->progress.total_known &&
	    fstatvfs(receiver->staging_fd, &space) == 0 &&
	    space.f_frsize != 0 &&
	    operation->progress.total_bytes / space.f_frsize +
			    (operation->progress.total_bytes % space.f_frsize !=
			     0) >
		    space.f_bavail) {
		fail(operation, ENOSPC, "not enough space for received file");
		operation->done = true;
		goto reject;
	}
	/* Precreate as the receiving user: the daemon truncates the same inode. */
	receiver->file_fd = openat(receiver->staging_fd, receiver->filename,
				   O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (receiver->file_fd < 0) {
		int saved_error = errno;
		fail(operation, saved_error, "cannot prepare received file: %s",
		     strerror(saved_error));
		operation->done = true;
		goto reject;
	}
	char target[PATH_MAX];
	if (!join_path(target, sizeof(target), receiver->staging,
		       receiver->filename) ||
	    snprintf(operation->transfer, sizeof(operation->transfer), "%s",
		     receiver->offered_transfer) >=
		    (int)sizeof(operation->transfer)) {
		fail(operation, ENAMETOOLONG, "receive path is too long");
		operation->done = true;
		goto reject;
	}
	DBusMessage *reply = dbus_message_new_method_return(message);
	const char *path = target;
	if (reply == NULL ||
	    !dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
				      DBUS_TYPE_INVALID)) {
		if (reply != NULL)
			dbus_message_unref(reply);
		fail(operation, ENOMEM,
		     "cannot form receive authorization reply");
		operation->done = true;
		goto reject;
	}
	receiver->authorized = true;
	agent_reply(operation->connection, reply);
	dbus_message_unref(receiver->authorization);
	receiver->authorization = NULL;
	report(operation, FPLINUX_BLUETOOTH_OPP_TRANSFERRING);
	return;
reject:
	agent_reply(operation->connection,
		    dbus_message_new_error(
			    message, "org.bluez.obex.Error.Rejected",
			    "push is not permitted by this receive window"));
	dbus_message_unref(receiver->authorization);
	receiver->authorization = NULL;
	if (!operation->done && !operation->owner_lost &&
	    !is_cancelled(operation)) {
		operation->progress.filename = "";
		report(operation, FPLINUX_BLUETOOTH_OPP_WAITING);
	}
}

static int rename_received(struct receiver *receiver, const char *destination,
			   bool replace)
{
	if (replace)
		return renameat(receiver->staging_fd, receiver->filename,
				receiver->directory_fd, destination);
#ifdef SYS_renameat2
	return (int)syscall(SYS_renameat2, receiver->staging_fd,
			    receiver->filename, receiver->directory_fd,
			    destination, RENAME_NOREPLACE);
#else
	errno = ENOSYS;
	return -1;
#endif
}

static int publish_received(struct receiver *receiver)
{
	struct operation *operation = &receiver->operation;
	struct fplinux_bluetooth_opp_result *result = operation->result;
	char destination[NAME_MAX + 1];
	if (!join_path(result->path, sizeof(result->path), receiver->staging,
		       receiver->filename))
		return fail(operation, ENAMETOOLONG,
			    "receive path is too long");
	if (fsync(receiver->file_fd) < 0) {
		int saved_error = errno;
		return fail(operation, saved_error,
			    "cannot sync received file: %s",
			    strerror(saved_error));
	}
	struct stat information;
	if (fstat(receiver->file_fd, &information) < 0) {
		int saved_error = errno;
		return fail(operation, saved_error,
			    "cannot inspect received file: %s",
			    strerror(saved_error));
	}
	operation->progress.transferred_bytes = (uint64_t)information.st_size;
	operation->progress.total_bytes = (uint64_t)information.st_size;
	operation->progress.total_known = true;
	if (is_cancelled(operation))
		return fail(operation, ECANCELED, "interrupted");
	report(operation, FPLINUX_BLUETOOTH_OPP_PUBLISHING);
	snprintf(destination, sizeof(destination), "%s", receiver->filename);
	if (rename_received(receiver, destination, false) < 0) {
		int saved_error = errno;
		if (saved_error != EEXIST)
			return fail(
				operation, saved_error,
				"cannot place received file without overwrite: %s",
				strerror(saved_error));
		enum fplinux_bluetooth_opp_conflict decision =
			FPLINUX_BLUETOOTH_OPP_KEEP_EXISTING;
		if (operation->callbacks != NULL &&
		    operation->callbacks->conflict != NULL)
			decision = operation->callbacks->conflict(
				receiver->filename, operation->callbacks->data);
		if (is_cancelled(operation))
			return fail(operation, ECANCELED, "interrupted");
		if (decision == FPLINUX_BLUETOOTH_OPP_REPLACE) {
			if (rename_received(receiver, destination, true) < 0) {
				saved_error = errno;
				return fail(operation, saved_error,
					    "cannot replace received file: %s",
					    strerror(saved_error));
			}
		} else if (decision == FPLINUX_BLUETOOTH_OPP_KEEP_BOTH) {
			unsigned int number = 1;
			const char *extension =
				strrchr(receiver->filename, '.');
			if (extension == NULL ||
			    extension == receiver->filename)
				extension = receiver->filename +
					    strlen(receiver->filename);
			int stem_size = (int)(extension - receiver->filename);
			for (;;) {
				if (snprintf(destination, sizeof(destination),
					     "%.*s (%u)%s", stem_size,
					     receiver->filename, number,
					     extension) >=
				    (int)sizeof(destination))
					return fail(
						operation, ENAMETOOLONG,
						"received filename is too long");
				if (rename_received(receiver, destination,
						    false) == 0)
					break;
				saved_error = errno;
				if (saved_error != EEXIST)
					return fail(
						operation, saved_error,
						"cannot place received file: %s",
						strerror(saved_error));
				if (is_cancelled(operation))
					return fail(operation, ECANCELED,
						    "interrupted");
				if (number == UINT_MAX)
					return fail(
						operation, EEXIST,
						"cannot choose an unused received filename");
				++number;
			}
		} else {
			return fail(operation, EEXIST,
				    "refusing to overwrite %s",
				    receiver->filename);
		}
	}
	result->published = true;
	join_path(result->path, sizeof(result->path), receiver->directory,
		  destination);
	if (fsync(receiver->staging_fd) < 0 ||
	    fsync(receiver->directory_fd) < 0) {
		int saved_error = errno;
		return fail(
			operation, saved_error,
			"received file is visible but directory sync failed: %s",
			strerror(saved_error));
	}
	result->outcome = FPLINUX_BLUETOOTH_OPP_SUCCEEDED;
	report(operation, FPLINUX_BLUETOOTH_OPP_COMPLETE);
	return 0;
}

static bool agent_method(struct receiver *receiver, const char *member,
			 const char *agent_path, bool cleanup)
{
	DBusMessage *message =
		method(OBEX_ROOT, "org.bluez.obex.AgentManager1", member);
	if (message != NULL &&
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH,
				      &agent_path, DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		message = NULL;
	}
	if (cleanup) {
		cleanup_call(&receiver->operation, message);
		return true;
	}
	DBusMessage *reply = call(&receiver->operation, message, false);
	if (reply == NULL)
		return false;
	dbus_message_unref(reply);
	return true;
}

int fplinux_bluetooth_opp_receive(
	const char *peer, const char *directory, int seconds,
	const struct fplinux_bluetooth_opp_callbacks *callbacks,
	struct fplinux_bluetooth_opp_result *result)
{
	*result = (struct fplinux_bluetooth_opp_result){ 0 };
	struct receiver receiver = {
		.operation = { .callbacks = callbacks,
			       .result = result,
			       .progress = { .peer = peer, .filename = "" },
			       .poll = poll_authorization },
		.directory_fd = -1,
		.staging_fd = -1,
		.file_fd = -1,
	};
	struct operation *operation = &receiver.operation;
	static const DBusObjectPathVTable vtable = { .message_function =
							     agent_message };
	const char *agent_path = "/org/fplinux/bluetooth/agent";
	int status = 1;
	if (seconds < 1 || seconds > 3600 || callbacks == NULL ||
	    callbacks->authorize == NULL)
		return fail(
			operation, EINVAL,
			"receive requires an authorization callback and a window of 1 to 3600 seconds");
	if (realpath(directory, receiver.directory) == NULL)
		return fail(operation, errno, "cannot resolve %s: %s",
			    directory, strerror(errno));
	receiver.directory_fd =
		open(receiver.directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (receiver.directory_fd < 0) {
		fail(operation, errno, "cannot open receive directory %s: %s",
		     directory, strerror(errno));
		goto out;
	}
	if (snprintf(receiver.staging, sizeof(receiver.staging),
		     "%s/.fplinux-bluetooth-XXXXXX",
		     receiver.directory) >= (int)sizeof(receiver.staging)) {
		receiver.staging[0] = '\0';
		fail(operation, ENAMETOOLONG,
		     "receive directory path is too long");
		goto out;
	}
	if (mkdtemp(receiver.staging) == NULL) {
		receiver.staging[0] = '\0';
		fail(operation, errno,
		     "cannot create private receive staging directory: %s",
		     strerror(errno));
		goto out;
	}
	receiver.staging_fd =
		open(receiver.staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (receiver.staging_fd < 0) {
		fail(operation, errno,
		     "cannot open receive staging directory: %s",
		     strerror(errno));
		goto out;
	}
	report(operation, FPLINUX_BLUETOOTH_OPP_CONNECTING);
	if (!open_bus(operation))
		goto out;
	if (!dbus_connection_register_object_path(
		    operation->connection, agent_path, &vtable, &receiver)) {
		fail(operation, ENOMEM,
		     "cannot register receive authorization agent");
		goto out;
	}
	receiver.object_registered = true;
	if (!agent_method(&receiver, "RegisterAgent", agent_path, false))
		goto out;
	receiver.agent_registered = true;
	if (!operation->done) {
		report(operation, FPLINUX_BLUETOOTH_OPP_WAITING);
	}
	status = wait_for_transfer(operation, seconds, true);
	if (status == 0)
		status = publish_received(&receiver);
out:
	if (receiver.authorization != NULL) {
		agent_reply(
			operation->connection,
			dbus_message_new_error(receiver.authorization,
					       "org.bluez.obex.Error.Canceled",
					       "receive window closed"));
		dbus_message_unref(receiver.authorization);
		receiver.authorization = NULL;
	}
	if (status != 0)
		cancel_transfer(operation);
	if (receiver.agent_registered)
		agent_method(&receiver, "UnregisterAgent", agent_path, true);
	if (receiver.object_registered)
		dbus_connection_unregister_object_path(operation->connection,
						       agent_path);
	close_bus(operation);
	if (receiver.file_fd >= 0)
		close(receiver.file_fd);
	/* Preserve complete files only when publication failed after transfer. */
	bool retained = operation->success && !result->published &&
			result->path[0] != '\0' &&
			result->outcome != FPLINUX_BLUETOOTH_OPP_CANCELLED;
	if (!retained && receiver.staging_fd >= 0 &&
	    receiver.filename[0] != '\0')
		unlinkat(receiver.staging_fd, receiver.filename, 0);
	if (receiver.staging_fd >= 0)
		close(receiver.staging_fd);
	if (!retained && receiver.staging[0] != '\0')
		rmdir(receiver.staging);
	if (receiver.directory_fd >= 0)
		close(receiver.directory_fd);
	if (!retained && !result->published)
		result->path[0] = '\0';
	return status;
}
