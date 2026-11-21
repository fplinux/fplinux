/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wifi-station.h"

#include <assert.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVICE "fi.w1.wpa_supplicant1"
#define ROOT_PATH "/fi/w1/wpa_supplicant1"
#define IFACE_PATH ROOT_PATH "/Interfaces/0"
#define FIRST_PATH IFACE_PATH "/Networks/0"
#define SECOND_PATH IFACE_PATH "/Networks/1"
#define PROPERTIES "org.freedesktop.DBus.Properties"

struct peer {
	const char *current;
	const char *state;
	bool has_interface;
	unsigned int added;
	char config[512];
	char tags[2][40];
};

static const char *processes;
static int net_namespace_error;

DIR *__real_opendir(const char *path);
int __real_stat(const char *path, struct stat *status);

DIR *__wrap_opendir(const char *path)
{
	return __real_opendir(!strcmp(path, "/proc") ? processes : path);
}

int __wrap_stat(const char *path, struct stat *status)
{
	if (net_namespace_error && !strcmp(path, "/proc/self/ns/net")) {
		errno = net_namespace_error;
		return -1;
	}
	return __real_stat(path, status);
}

static const char *dictionary_string(DBusMessage *message, const char *key)
{
	DBusMessageIter args, dictionary;

	assert(dbus_message_has_signature(message, "a{sv}"));
	assert(dbus_message_iter_init(message, &args));
	dbus_message_iter_recurse(&args, &dictionary);
	while (dbus_message_iter_get_arg_type(&dictionary) ==
	       DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *name, *value;

		dbus_message_iter_recurse(&dictionary, &entry);
		dbus_message_iter_get_basic(&entry, &name);
		if (!strcmp(name, key)) {
			assert(dbus_message_iter_next(&entry));
			dbus_message_iter_recurse(&entry, &variant);
			assert(dbus_message_iter_get_arg_type(&variant) ==
			       DBUS_TYPE_STRING);
			dbus_message_iter_get_basic(&variant, &value);
			return value;
		}
		dbus_message_iter_next(&dictionary);
	}
	return NULL;
}

static void add_property(DBusMessageIter *dictionary, const char *name,
			 const char *type, int basic, const char *value)
{
	DBusMessageIter entry, variant;

	assert(dbus_message_iter_open_container(
		dictionary, DBUS_TYPE_DICT_ENTRY, NULL, &entry));
	assert(dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name));
	assert(dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, type,
						&variant));
	assert(dbus_message_iter_append_basic(&variant, basic, &value));
	assert(dbus_message_iter_close_container(&entry, &variant));
	assert(dbus_message_iter_close_container(dictionary, &entry));
}

static void add_state(DBusMessage *reply, const struct peer *peer)
{
	DBusMessageIter args, dictionary;

	dbus_message_iter_init_append(reply, &args);
	assert(dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}",
						&dictionary));
	add_property(&dictionary, "State", "s", DBUS_TYPE_STRING, peer->state);
	add_property(&dictionary, "CurrentNetwork", "o", DBUS_TYPE_OBJECT_PATH,
		     peer->current);
	assert(dbus_message_iter_close_container(&args, &dictionary));
}

static void add_empty_networks(DBusMessage *reply)
{
	DBusMessageIter args, variant, array;

	dbus_message_iter_init_append(reply, &args);
	assert(dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "ao",
						&variant));
	assert(dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "o",
						&array));
	assert(dbus_message_iter_close_container(&variant, &array));
	assert(dbus_message_iter_close_container(&args, &variant));
}

static DBusMessage *handle_request(DBusMessage *request, struct peer *peer)
{
	const char *method = dbus_message_get_member(request);
	DBusMessage *reply;
	const char *path;

	if (!strcmp(method, "GetInterface") && !peer->has_interface)
		return dbus_message_new_error(request,
					      SERVICE ".InterfaceUnknown",
					      "WLAN is unmanaged");
	reply = dbus_message_new_method_return(request);
	assert(reply);
	if (!strcmp(method, "CreateInterface")) {
		const char *config = dictionary_string(request, "ConfigFile");

		assert(!peer->has_interface);
		assert(!strcmp(dictionary_string(request, "Ifname"),
			       "testwlan"));
		assert(!strcmp(dictionary_string(request, "Driver"),
			       "nl80211"));
		assert(config && strlen(config) < sizeof(peer->config));
		strcpy(peer->config, config);
		peer->has_interface = true;
		path = IFACE_PATH;
		assert(dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH,
						&path, DBUS_TYPE_INVALID));
	} else if (!strcmp(method, "GetInterface")) {
		path = IFACE_PATH;
		assert(dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH,
						&path, DBUS_TYPE_INVALID));
	} else if (!strcmp(method, "Get")) {
		const char *interface, *property;

		assert(dbus_message_get_args(request, NULL, DBUS_TYPE_STRING,
					     &interface, DBUS_TYPE_STRING,
					     &property, DBUS_TYPE_INVALID));
		assert(!strcmp(interface, SERVICE ".Interface"));
		assert(!strcmp(property, "Networks"));
		add_empty_networks(reply);
	} else if (!strcmp(method, "GetAll")) {
		add_state(reply, peer);
	} else if (!strcmp(method, "AddNetwork")) {
		const char *tag = dictionary_string(request, "id_str");

		assert(peer->added < 2);
		assert(tag && strlen(tag) < sizeof(peer->tags[0]));
		strcpy(peer->tags[peer->added], tag);
		assert(!strcmp(dictionary_string(request, "key_mgmt"),
			       "WPA-PSK"));
		assert(!strcmp(dictionary_string(request, "proto"), "RSN"));
		assert(!strcmp(dictionary_string(request, "pairwise"), "CCMP"));
		assert(!strcmp(dictionary_string(request, "group"), "CCMP"));
		path = peer->added++ ? SECOND_PATH : FIRST_PATH;
		assert(dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH,
						&path, DBUS_TYPE_INVALID));
	} else if (!strcmp(method, "SelectNetwork")) {
		assert(dbus_message_get_args(
			request, NULL, DBUS_TYPE_OBJECT_PATH, &peer->current,
			DBUS_TYPE_INVALID));
		if (!strcmp(peer->current, FIRST_PATH)) {
			peer->current = FIRST_PATH;
			peer->state = "associating";
		} else {
			assert(!strcmp(peer->current, SECOND_PATH));
			peer->current = SECOND_PATH;
			peer->state = "completed";
		}
	} else if (!strcmp(method, "Set")) {
		DBusMessageIter args, value;
		const char *interface, *property;
		dbus_bool_t enabled;

		/* A delayed old completion after cancellation cannot finish a request. */
		assert(dbus_message_has_interface(request, PROPERTIES));
		assert(dbus_message_has_path(request, FIRST_PATH));
		assert(dbus_message_has_signature(request, "ssv"));
		assert(dbus_message_iter_init(request, &args));
		dbus_message_iter_get_basic(&args, &interface);
		assert(!strcmp(interface, SERVICE ".Network"));
		assert(dbus_message_iter_next(&args));
		dbus_message_iter_get_basic(&args, &property);
		assert(!strcmp(property, "Enabled"));
		assert(dbus_message_iter_next(&args));
		dbus_message_iter_recurse(&args, &value);
		assert(dbus_message_iter_get_arg_type(&value) ==
		       DBUS_TYPE_BOOLEAN);
		dbus_message_iter_get_basic(&value, &enabled);
		assert(!enabled);
		peer->state = "completed";
	} else if (!strcmp(method, "SaveConfig")) {
		char temporary[sizeof(peer->config) + 4];
		FILE *file;

		assert(!strcmp(peer->current, SECOND_PATH) && peer->added == 2);
		assert(snprintf(temporary, sizeof(temporary), "%s.tmp",
				peer->config) < (int)sizeof(temporary));
		file = fopen(temporary, "w");
		assert(file);
		assert(fprintf(file,
			       "update_config=1\np2p_disabled=1\n"
			       "\nnetwork={\n\tssid=\"test\"\n\tpsk=\"rejected-pass\"\n"
			       "\tdisabled=1\n\tid_str=\"%s\"\n}\n"
			       "\nnetwork={\n\tssid=\"test\"\n\tpsk=\"accepted-pass\"\n"
			       "\tid_str=\"%s\"\n}\n",
			       peer->tags[0], peer->tags[1]) > 0);
		assert(fclose(file) == 0);
		assert(rename(temporary, peer->config) == 0);
	} else if (!strcmp(method, "RemoveInterface")) {
		assert(peer->has_interface);
		peer->has_interface = false;
	} else {
		assert(!"unexpected D-Bus method");
	}
	return reply;
}

static void serve_peer(int ready)
{
	struct peer peer = {
		.current = "/",
		.state = "inactive",
	};
	DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);

	assert(prctl(PR_SET_NAME, "wpa_supplicant", 0L, 0L, 0L) == 0);
	assert(bus);
	assert(dbus_bus_request_name(bus, SERVICE, 0, NULL) ==
	       DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
	assert(write(ready, "1", 1) == 1);
	close(ready);
	while (dbus_connection_read_write(bus, 10)) {
		DBusMessage *message = dbus_connection_pop_message(bus);

		if (!message)
			continue;
		if (dbus_message_get_type(message) ==
		    DBUS_MESSAGE_TYPE_METHOD_CALL) {
			DBusMessage *reply = handle_request(message, &peer);

			assert(dbus_connection_send(bus, reply, NULL));
			dbus_connection_flush(bus);
			dbus_message_unref(reply);
		}
		dbus_message_unref(message);
	}
}

static void check_station(const char *config)
{
	struct fplinux_session_wifi_station *station, *observer;
	struct fplinux_ui_wifi_request connect = {
		.action = FPLINUX_UI_WIFI_CONNECT_HIDDEN,
		.security = FPLINUX_UI_WIFI_WPA2,
		.ssid = "test",
		.save = true,
	};
	struct fplinux_ui_wifi_request cancel = {
		.action = FPLINUX_UI_WIFI_CANCEL,
		.target = 1,
	};
	enum fplinux_ui_wifi_result result;
	const struct fplinux_ui_wifi_state *state;
	struct stat status;
	uint32_t serial;

	station = fplinux_session_wifi_station_create("testwlan", config);
	observer = fplinux_session_wifi_station_create("testwlan", config);
	assert(station && observer);
	result = fplinux_session_wifi_station_acquire(station);
	if (result != FPLINUX_UI_WIFI_APPLIED)
		fprintf(stderr, "station acquisition result: %d\n", result);
	assert(result == FPLINUX_UI_WIFI_APPLIED);
	assert(fplinux_session_wifi_station_acquire(observer) ==
	       FPLINUX_UI_WIFI_BUSY);
	fplinux_session_wifi_station_destroy(observer);
	strcpy(connect.key, "rejected-pass");
	assert(!fplinux_session_wifi_station_request(station, &connect, 1,
						     &result));
	assert(fplinux_session_wifi_station_request(station, &cancel, 2,
						    &result));
	assert(result == FPLINUX_UI_WIFI_APPLIED);
	fplinux_session_wifi_station_update(station);
	assert(!fplinux_session_wifi_station_take_result(station, &serial,
							 &result));
	state = fplinux_session_wifi_station_state(station);
	assert(state->link == FPLINUX_UI_WIFI_DISCONNECTED &&
	       !state->saved_count);
	assert(stat(config, &status) == -1);

	strcpy(connect.key, "accepted-pass");
	assert(!fplinux_session_wifi_station_request(station, &connect, 3,
						     &result));
	fplinux_session_wifi_station_update(station);
	assert(fplinux_session_wifi_station_take_result(station, &serial,
							&result));
	assert(serial == 3 && result == FPLINUX_UI_WIFI_APPLIED);
	assert(state->link == FPLINUX_UI_WIFI_ADDRESSING);
	assert(!state->saved_count && !state->current.profile_id);
	assert(stat(config, &status) == -1);
	assert(fplinux_session_wifi_station_commit(station) ==
	       FPLINUX_UI_WIFI_APPLIED);
	assert(state->saved_count == 1 &&
	       state->current.profile_id == state->saved[0].id);
	assert(stat(config, &status) == 0 && (status.st_mode & 0777) == 0600);
	fplinux_session_wifi_station_destroy(station);
}

static void check_station_without_netns(const char *config, pid_t peer)
{
	struct fplinux_session_wifi_station *station;
	char owner_path[PATH_MAX], manual_path[PATH_MAX];

	station = fplinux_session_wifi_station_create("testwlan", config);
	assert(station);
	assert(snprintf(owner_path, sizeof(owner_path), "%s/%ld", processes,
			(long)peer) < (int)sizeof(owner_path));
	assert(mkdir(owner_path, 0700) == 0);

	net_namespace_error = EACCES;
	assert(fplinux_session_wifi_station_acquire(station) ==
	       FPLINUX_UI_WIFI_BUSY);

	net_namespace_error = ENOENT;
	assert(prctl(PR_SET_NAME, "hostapd", 0L, 0L, 0L) == 0);
	assert(snprintf(manual_path, sizeof(manual_path), "%s/%ld", processes,
			(long)getpid()) < (int)sizeof(manual_path));
	assert(mkdir(manual_path, 0700) == 0);
	assert(fplinux_session_wifi_station_acquire(station) ==
	       FPLINUX_UI_WIFI_BUSY);
	assert(rmdir(manual_path) == 0);

	assert(fplinux_session_wifi_station_acquire(station) ==
	       FPLINUX_UI_WIFI_APPLIED);
	fplinux_session_wifi_station_destroy(station);
}

int main(int argc, char **argv)
{
	int ready[2], status;
	pid_t peer;
	char started;

	assert(argc == 4);
	processes = argv[2];
	assert(setenv("DBUS_SYSTEM_BUS_ADDRESS",
		      getenv("DBUS_SESSION_BUS_ADDRESS"), 1) == 0);
	assert(pipe(ready) == 0);
	peer = fork();
	assert(peer >= 0);
	if (peer == 0) {
		close(ready[0]);
		serve_peer(ready[1]);
		_exit(0);
	}
	close(ready[1]);
	assert(read(ready[0], &started, 1) == 1 && started == '1');
	close(ready[0]);
	if (!strcmp(argv[3], "no-netns")) {
		check_station_without_netns(argv[1], peer);
	} else {
		assert(!strcmp(argv[3], "cancel"));
		check_station(argv[1]);
	}
	assert(kill(peer, SIGTERM) == 0);
	assert(waitpid(peer, &status, 0) == peer);
	assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
	return 0;
}
