/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-wifi-station.h"

#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-utf8.h"

#define WPA_SERVICE "fi.w1.wpa_supplicant1"
#define WPA_ROOT "/fi/w1/wpa_supplicant1"
#define WPA_INTERFACE WPA_SERVICE ".Interface"
#define WPA_NETWORK WPA_SERVICE ".Network"
#define WPA_BSS WPA_SERVICE ".BSS"
#define WPA_PROPERTIES "org.freedesktop.DBus.Properties"
#define WPA_PATH_BYTES 150
#define WPA_TIMEOUT_MS 350
#define SCAN_TIMEOUT_MS 15000
#define CONNECT_TIMEOUT_MS 20000
#define LINK_POLL_MS 250
#define IDLE_POLL_MS 2000
#define SCAN_RESULTS_PER_UPDATE 4
#define WIFI_CONFIG_PATH "/var/lib/fplinux/wifi/wpa_supplicant.conf"
#define WIFI_RUNTIME_TEMPLATE "/run/fplinux-gui/compositor/wifi-XXXXXX"
#define WIFI_PROFILE_TAG "fplinux-"
#define WIFI_NETWORK_BLOCK_BYTES 1024

static const char config_defaults[] = "update_config=1\np2p_disabled=1\n";
static const char hex_digits[] = "0123456789abcdef";

struct wifi_ssid {
	unsigned char bytes[FPLINUX_UI_WIFI_SSID_BYTES];
	size_t length;
};

struct wifi_network {
	struct wifi_ssid ssid;
	enum fplinux_ui_wifi_security security;
	uint32_t id;
};

struct wifi_bss {
	struct wifi_network network;
	unsigned char bssid[6];
	dbus_int16_t signal;
};

struct wifi_profile {
	struct wifi_network network;
	char path[WPA_PATH_BYTES];
	bool hidden;
};

struct wifi_selection {
	struct wifi_network network;
	char path[WPA_PATH_BYTES];
	uint32_t profile_id;
	bool save;
	bool hidden;
	bool reached_authentication;
	bool reached_handshake;
	bool psk_mismatch;
};

struct fplinux_session_wifi_station {
	char ifname[IFNAMSIZ];
	char config_path[PATH_MAX];
	char runtime_path[PATH_MAX];
	char working_path[PATH_MAX];
	char working_tmp[PATH_MAX];
	DBusConnection *bus;
	char owner[WPA_PATH_BYTES];
	char interface[WPA_PATH_BYTES];
	struct fplinux_ui_wifi_state state;
	struct wifi_bss available[FPLINUX_UI_WIFI_ROW_COUNT];
	struct wifi_bss scanned[FPLINUX_UI_WIFI_ROW_COUNT];
	size_t scanned_count;
	struct wifi_profile profiles[FPLINUX_UI_WIFI_ROW_COUNT];
	size_t profile_count;
	struct wifi_selection selected;
	char retired_path[WPA_PATH_BYTES];
	uint32_t pending_serial;
	uint32_t ready_serial;
	enum fplinux_ui_wifi_result ready_result;
	uint64_t deadline_ms;
	uint64_t next_poll_ms;
	DBusMessage *scan_reply;
	DBusMessageIter scan_paths;
	bool scan_waiting;
	uint32_t scan_reply_serial;
	uint32_t select_reply_serial;
	unsigned int missing_updates;
};

static atomic_uint next_network_id;

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t new_network_id(void)
{
	uint32_t id;

	do {
		id = atomic_fetch_add_explicit(&next_network_id, 1,
					       memory_order_relaxed) +
		     1;
	} while (!id);
	return id;
}

static bool copy_string(char *destination, size_t size, const char *text)
{
	size_t length = strlen(text);

	if (length >= size)
		return false;
	memcpy(destination, text, length + 1);
	return true;
}

static void hex_byte(char *text, unsigned char byte)
{
	text[0] = hex_digits[byte >> 4];
	text[1] = hex_digits[byte & 15];
}

static int hex_value(unsigned char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

static bool decode_hex(const char *text, unsigned char *bytes, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		int high = hex_value(text[2 * i]);
		int low = hex_value(text[2 * i + 1]);

		if (high < 0 || low < 0)
			return false;
		bytes[i] = (high << 4) | low;
	}
	return true;
}

static bool same_network(const struct wifi_network *a,
			 const struct wifi_network *b)
{
	return a->security == b->security && a->ssid.length == b->ssid.length &&
	       !memcmp(a->ssid.bytes, b->ssid.bytes, a->ssid.length);
}

static void display_ssid(char *display, const struct wifi_ssid *ssid)
{
	const unsigned char *cursor = ssid->bytes;
	const unsigned char *end = ssid->bytes + ssid->length;

	while (cursor != end) {
		const unsigned char *start = cursor;
		uint32_t scalar;

		if (fplinux_utf8_next(&cursor, end, &scalar) &&
		    scalar >= 0x20 && (scalar < 0x7f || scalar > 0x9f) &&
		    scalar != '\\' && !(scalar >= 0x202a && scalar <= 0x202e) &&
		    !(scalar >= 0x2066 && scalar <= 0x2069)) {
			memcpy(display, start, cursor - start);
			display += cursor - start;
			continue;
		}
		cursor = start + 1;
		*display++ = '\\';
		*display++ = 'x';
		hex_byte(display, *start);
		display += 2;
	}
	*display = '\0';
}

static const struct wifi_profile *
find_profile(const struct fplinux_session_wifi_station *station, uint32_t id)
{
	for (size_t i = 0; i < station->profile_count; ++i)
		if (station->profiles[i].network.id == id)
			return &station->profiles[i];
	return NULL;
}

static uint32_t
matching_profile(const struct fplinux_session_wifi_station *station,
		 const struct wifi_network *network)
{
	for (size_t i = 0; i < station->profile_count; ++i)
		if (same_network(&station->profiles[i].network, network))
			return station->profiles[i].network.id;
	return 0;
}

static void present_network(struct fplinux_ui_wifi_network *row,
			    const struct wifi_network *network,
			    uint32_t profile_id)
{
	row->id = network->id;
	row->security = network->security;
	row->profile_id = profile_id;
	display_ssid(row->name, &network->ssid);
}

static void present_scan(struct fplinux_session_wifi_station *station)
{
	memset(station->state.available, 0, sizeof(station->state.available));
	for (size_t i = 0; i < station->state.available_count; ++i) {
		const struct wifi_network *network =
			&station->available[i].network;

		present_network(&station->state.available[i], network,
				matching_profile(station, network));
	}
}

static void present_profiles(struct fplinux_session_wifi_station *station)
{
	station->state.saved_count = station->profile_count;
	memset(station->state.saved, 0, sizeof(station->state.saved));
	for (size_t i = 0; i < station->profile_count; ++i) {
		const struct wifi_network *network =
			&station->profiles[i].network;

		present_network(&station->state.saved[i], network, network->id);
	}
	present_scan(station);
}

static DBusMessage *call(struct fplinux_session_wifi_station *station,
			 DBusMessage *message, DBusError *error)
{
	DBusMessage *reply;

	if (!message)
		return NULL;
	reply = dbus_connection_send_with_reply_and_block(
		station->bus, message, WPA_TIMEOUT_MS, error);
	dbus_message_unref(message);
	return reply;
}

static DBusMessage *new_call(struct fplinux_session_wifi_station *station,
			     const char *path, const char *interface,
			     const char *method)
{
	return dbus_message_new_method_call(station->owner, path, interface,
					    method);
}

static bool path_call(struct fplinux_session_wifi_station *station,
		      const char *path, const char *interface,
		      const char *method, const char *argument)
{
	DBusMessage *message = new_call(station, path, interface, method);
	DBusMessage *reply;

	if (!message)
		return false;
	if (argument &&
	    !dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &argument,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		return false;
	}
	reply = call(station, message, NULL);
	if (!reply)
		return false;
	dbus_message_unref(reply);
	return true;
}

static bool open_dictionary(DBusMessageIter *parent, DBusMessageIter *dict)
{
	return dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}",
						dict);
}

static bool append_value(DBusMessageIter *parent, const char *key,
			 const char *signature, int type, const void *value,
			 size_t count)
{
	DBusMessageIter entry, variant, array;
	bool appended;

	if (!dbus_message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY,
					      NULL, &entry) ||
	    !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
	    !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					      signature, &variant))
		return false;
	if (type == DBUS_TYPE_ARRAY) {
		appended = dbus_message_iter_open_container(
				   &variant, DBUS_TYPE_ARRAY, "y", &array) &&
			   dbus_message_iter_append_fixed_array(
				   &array, DBUS_TYPE_BYTE, &value, count) &&
			   dbus_message_iter_close_container(&variant, &array);
	} else {
		appended =
			dbus_message_iter_append_basic(&variant, type, value);
	}
	return appended &&
	       dbus_message_iter_close_container(&entry, &variant) &&
	       dbus_message_iter_close_container(parent, &entry);
}

static bool append_text(DBusMessageIter *dict, const char *key,
			const char *value)
{
	return append_value(dict, key, "s", DBUS_TYPE_STRING, &value, 0);
}

static bool get_field(DBusMessageIter dict, const char *key, int type,
		      DBusMessageIter *value)
{
	DBusMessageIter entries;

	if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_ARRAY)
		return false;
	dbus_message_iter_recurse(&dict, &entries);
	while (dbus_message_iter_get_arg_type(&entries) ==
	       DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *name;

		dbus_message_iter_recurse(&entries, &entry);
		dbus_message_iter_get_basic(&entry, &name);
		if (!strcmp(name, key)) {
			dbus_message_iter_next(&entry);
			dbus_message_iter_recurse(&entry, value);
			return dbus_message_iter_get_arg_type(value) == type;
		}
		dbus_message_iter_next(&entries);
	}
	return false;
}

static bool get_basic(DBusMessageIter dict, const char *key, int type,
		      void *value)
{
	DBusMessageIter field;

	if (!get_field(dict, key, type, &field))
		return false;
	dbus_message_iter_get_basic(&field, value);
	return true;
}

static const char *get_text(DBusMessageIter dict, const char *key)
{
	const char *value = "";

	get_basic(dict, key, DBUS_TYPE_STRING, &value);
	return value;
}

static bool get_bytes(DBusMessageIter dict, const char *key, void *value,
		      size_t capacity, size_t *length)
{
	DBusMessageIter field, array;
	const unsigned char *bytes;
	int count;

	if (!get_field(dict, key, DBUS_TYPE_ARRAY, &field) ||
	    dbus_message_iter_get_element_type(&field) != DBUS_TYPE_BYTE)
		return false;
	dbus_message_iter_recurse(&field, &array);
	dbus_message_iter_get_fixed_array(&array, &bytes, &count);
	if (count < 1 || (size_t)count > capacity)
		return false;
	memcpy(value, bytes, count);
	*length = count;
	return true;
}

static DBusMessage *get_properties(struct fplinux_session_wifi_station *station,
				   const char *path, const char *interface,
				   const char *property)
{
	DBusMessage *message = new_call(station, path, WPA_PROPERTIES,
					property ? "Get" : "GetAll");

	if (!message)
		return NULL;
	if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &interface,
				      property ? DBUS_TYPE_STRING :
						 DBUS_TYPE_INVALID,
				      &property, DBUS_TYPE_INVALID)) {
		dbus_message_unref(message);
		return NULL;
	}
	return call(station, message, NULL);
}

static DBusMessage *
network_properties(struct fplinux_session_wifi_station *station,
		   const char *path, DBusMessageIter *properties)
{
	DBusMessage *reply =
		get_properties(station, path, WPA_NETWORK, "Properties");
	DBusMessageIter value;

	if (!reply)
		return NULL;
	if (!dbus_message_has_signature(reply, "v") ||
	    !dbus_message_iter_init(reply, &value)) {
		dbus_message_unref(reply);
		return NULL;
	}
	dbus_message_iter_recurse(&value, properties);
	return reply;
}

static bool set_enabled(struct fplinux_session_wifi_station *station,
			const char *path, dbus_bool_t enabled)
{
	const char *interface = WPA_NETWORK, *property = "Enabled";
	DBusMessage *message = new_call(station, path, WPA_PROPERTIES, "Set");
	DBusMessageIter args, variant;
	DBusMessage *reply;

	if (!message)
		return false;
	dbus_message_iter_init_append(message, &args);
	if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING,
					    &interface) ||
	    !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING,
					    &property) ||
	    !dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b",
					      &variant) ||
	    !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN,
					    &enabled) ||
	    !dbus_message_iter_close_container(&args, &variant)) {
		dbus_message_unref(message);
		return false;
	}
	reply = call(station, message, NULL);
	if (!reply)
		return false;
	dbus_message_unref(reply);
	return true;
}

static bool remove_network(struct fplinux_session_wifi_station *station,
			   const char *path)
{
	return path_call(station, station->interface, WPA_INTERFACE,
			 "RemoveNetwork", path);
}

static bool owns_interface(const struct fplinux_session_wifi_station *station)
{
	return station->interface[0];
}

/* Selection paths created by this backend are retired until a new one exists. */
static bool release_selection(struct fplinux_session_wifi_station *station)
{
	if (station->selected.path[0]) {
		if (!set_enabled(station, station->selected.path, FALSE))
			return false;
		if (!find_profile(station, station->selected.network.id)) {
			if (station->retired_path[0])
				remove_network(station, station->retired_path);
			strcpy(station->retired_path, station->selected.path);
		}
	}
	memset(&station->selected, 0, sizeof(station->selected));
	memset(&station->state.current, 0, sizeof(station->state.current));
	station->state.link = FPLINUX_UI_WIFI_DISCONNECTED;
	return true;
}

static void remove_working_config(struct fplinux_session_wifi_station *station)
{
	if (station->working_tmp[0])
		unlink(station->working_tmp);
	if (station->working_path[0])
		unlink(station->working_path);
	if (station->runtime_path[0])
		rmdir(station->runtime_path);
	station->working_tmp[0] = '\0';
	station->working_path[0] = '\0';
	station->runtime_path[0] = '\0';
}

static void release_interface(struct fplinux_session_wifi_station *station)
{
	if (owns_interface(station))
		path_call(station, WPA_ROOT, WPA_SERVICE, "RemoveInterface",
			  station->interface);
	if (station->scan_reply) {
		dbus_message_unref(station->scan_reply);
		station->scan_reply = NULL;
	}
	if (station->bus) {
		dbus_connection_close(station->bus);
		dbus_connection_unref(station->bus);
		station->bus = NULL;
	}
	remove_working_config(station);
	station->interface[0] = '\0';
	station->owner[0] = '\0';
	station->retired_path[0] = '\0';
	station->profile_count = 0;
	station->scanned_count = 0;
	station->scan_waiting = false;
	station->missing_updates = 0;
	memset(&station->selected, 0, sizeof(station->selected));
	station->state = (struct fplinux_ui_wifi_state){
		.power = FPLINUX_UI_WIFI_ON,
	};
}

static bool config_directory(struct fplinux_session_wifi_station *station,
			     char path[PATH_MAX])
{
	struct stat status;
	char *basename;

	strcpy(path, station->config_path);
	basename = strrchr(path, '/');
	if (!basename || basename == path)
		return false;
	*basename = '\0';
	if (!strcmp(station->config_path, WIFI_CONFIG_PATH) &&
	    mkdir(path, 0700) < 0 && errno != EEXIST)
		return false;
	return lstat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
	       status.st_uid == geteuid() && (status.st_mode & 0777) == 0700;
}

static bool copy_config(FILE *input, FILE *output)
{
	char buffer[4096];
	size_t count;
	bool ok = true;

	do {
		count = fread(buffer, 1, sizeof(buffer), input);
		if (count && fwrite(buffer, 1, count, output) != count) {
			ok = false;
			break;
		}
	} while (count == sizeof(buffer));
	explicit_bzero(buffer, sizeof(buffer));
	return ok && !ferror(input);
}

static bool prepare_working_config(struct fplinux_session_wifi_station *station)
{
	char directory[PATH_MAX];
	char runtime_path[PATH_MAX];
	struct stat status;
	int config_fd, working_fd;
	FILE *input = NULL, *output;
	bool ok;

	if (!config_directory(station, directory))
		return false;
	if (!strcmp(station->config_path, WIFI_CONFIG_PATH)) {
		strcpy(runtime_path, WIFI_RUNTIME_TEMPLATE);
	} else if (snprintf(runtime_path, sizeof(runtime_path),
			    "%s/.wifi-XXXXXX",
			    directory) >= (int)sizeof(runtime_path)) {
		return false;
	}
	if (!mkdtemp(runtime_path))
		return false;
	strcpy(station->runtime_path, runtime_path);
	if (snprintf(station->working_path, sizeof(station->working_path),
		     "%s/wpa.conf",
		     runtime_path) >= (int)sizeof(station->working_path) ||
	    snprintf(station->working_tmp, sizeof(station->working_tmp),
		     "%s/wpa.conf.tmp",
		     runtime_path) >= (int)sizeof(station->working_tmp))
		return false;

	config_fd =
		open(station->config_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (config_fd >= 0) {
		if (fstat(config_fd, &status) < 0 || !S_ISREG(status.st_mode) ||
		    status.st_uid != geteuid() ||
		    (status.st_mode & 0777) != 0600 ||
		    !(input = fdopen(config_fd, "r"))) {
			close(config_fd);
			return false;
		}
	} else if (errno != ENOENT) {
		return false;
	}
	working_fd = open(station->working_path,
			  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	output = working_fd >= 0 ? fdopen(working_fd, "w") : NULL;
	if (output) {
		ok = fchmod(working_fd, 0600) == 0 &&
		     (input ? copy_config(input, output) :
			      fputs(config_defaults, output) >= 0);
		ok = fclose(output) == 0 && ok;
	} else {
		if (working_fd >= 0)
			close(working_fd);
		ok = false;
	}
	if (input)
		fclose(input);
	return ok;
}

static uint32_t profile_tag_id(const char *tag)
{
	uint32_t id = 0;
	size_t prefix = sizeof(WIFI_PROFILE_TAG) - 1;

	if (strlen(tag) != prefix + 10 || tag[0] != '"' ||
	    strncmp(tag + 1, WIFI_PROFILE_TAG, prefix) ||
	    tag[prefix + 9] != '"')
		return 0;
	tag += prefix + 1;
	for (size_t i = 0; i < 8; ++i) {
		int digit = hex_value(tag[i]);

		if (digit < 0)
			return 0;
		id = (id << 4) | digit;
	}
	return id;
}

static uint32_t config_line_tag(char *line, size_t bytes)
{
	static const char prefix[] = "\tid_str=";
	uint32_t id;

	if (strncmp(line, prefix, sizeof(prefix) - 1))
		return 0;
	line[bytes - 1] = '\0';
	id = profile_tag_id(line + sizeof(prefix) - 1);
	line[bytes - 1] = '\n';
	return id;
}

static bool profile_ssid(const char *value, struct wifi_ssid *ssid)
{
	size_t length = strlen(value);

	if (value[0] == '"' && length >= 2 && value[length - 1] == '"') {
		length -= 2;
		if (!length || length > sizeof(ssid->bytes))
			return false;
		memcpy(ssid->bytes, value + 1, length);
	} else {
		if (length % 2)
			return false;
		length /= 2;
		if (!length || length > sizeof(ssid->bytes) ||
		    !decode_hex(value, ssid->bytes, length))
			return false;
	}
	ssid->length = length;
	return true;
}

static bool read_profile(struct fplinux_session_wifi_station *station,
			 const char *path)
{
	struct wifi_profile profile = { 0 };
	DBusMessageIter properties;
	DBusMessage *reply = network_properties(station, path, &properties);
	uint32_t id;
	bool valid;

	if (!reply)
		return false;
	id = profile_tag_id(get_text(properties, "id_str"));
	valid = id && station->profile_count < FPLINUX_UI_WIFI_ROW_COUNT &&
		!find_profile(station, id) &&
		profile_ssid(get_text(properties, "ssid"),
			     &profile.network.ssid) &&
		copy_string(profile.path, sizeof(profile.path), path) &&
		!strcmp(get_text(properties, "key_mgmt"), "WPA-PSK") &&
		!strcmp(get_text(properties, "proto"), "RSN") &&
		!strcmp(get_text(properties, "pairwise"), "CCMP") &&
		!strcmp(get_text(properties, "group"), "CCMP");
	profile.hidden = !strcmp(get_text(properties, "scan_ssid"), "1");
	dbus_message_unref(reply);
	if (!valid)
		return false;
	profile.network.id = id;
	profile.network.security = FPLINUX_UI_WIFI_WPA2;
	station->profiles[station->profile_count++] = profile;
	if (id > atomic_load_explicit(&next_network_id, memory_order_relaxed))
		atomic_store_explicit(&next_network_id, id,
				      memory_order_relaxed);
	return true;
}

static bool load_profiles(struct fplinux_session_wifi_station *station)
{
	DBusMessage *reply = get_properties(station, station->interface,
					    WPA_INTERFACE, "Networks");
	DBusMessageIter value, array, paths;
	bool ok = false;

	if (!reply)
		return false;
	if (!dbus_message_has_signature(reply, "v") ||
	    !dbus_message_iter_init(reply, &value))
		goto done;
	dbus_message_iter_recurse(&value, &array);
	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY ||
	    dbus_message_iter_get_element_type(&array) != DBUS_TYPE_OBJECT_PATH)
		goto done;
	dbus_message_iter_recurse(&array, &paths);
	while (dbus_message_iter_get_arg_type(&paths) ==
	       DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&paths, &path);
		if (!read_profile(station, path))
			goto done;
		dbus_message_iter_next(&paths);
	}
	ok = true;
done:
	dbus_message_unref(reply);
	return ok;
}

static DBusMessage *bus_name_call(struct fplinux_session_wifi_station *station,
				  const char *method, const char *name)
{
	DBusMessage *request = dbus_message_new_method_call(
		DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, method);

	if (!request)
		return NULL;
	if (!dbus_message_append_args(request, DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(request);
		return NULL;
	}
	return call(station, request, NULL);
}

static bool get_supplicant_owner(struct fplinux_session_wifi_station *station)
{
	DBusMessage *reply;
	const char *owner;
	bool available = false;

	reply = bus_name_call(station, "GetNameOwner", WPA_SERVICE);
	if (reply) {
		available = dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING,
						  &owner, DBUS_TYPE_INVALID) &&
			    owner[0] == ':' &&
			    copy_string(station->owner, sizeof(station->owner),
					owner);
		dbus_message_unref(reply);
	}
	return available;
}

static bool has_manual_manager(struct fplinux_session_wifi_station *station)
{
	DBusMessage *reply;
	dbus_uint32_t owner_pid = 0;
	struct dirent *entry;
	struct stat own_namespace;
	DIR *processes;
	bool found = false;

	reply = bus_name_call(station, "GetConnectionUnixProcessID",
			      station->owner);
	if (reply) {
		dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &owner_pid,
				      DBUS_TYPE_INVALID);
		dbus_message_unref(reply);
	}
	if (stat("/proc/self/ns/net", &own_namespace) < 0)
		return true;
	processes = opendir("/proc");
	if (!processes)
		return true;
	while ((entry = readdir(processes)) != NULL) {
		char path[sizeof("/proc//ns/net") + sizeof(entry->d_name)];
		char name[32];
		char *end;
		unsigned long pid = strtoul(entry->d_name, &end, 10);
		struct stat peer_namespace;
		FILE *comm;

		if (!pid || *end || pid == owner_pid)
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		comm = fopen(path, "re");
		if (!comm)
			continue;
		found = fgets(name, sizeof(name), comm) &&
			(!strcmp(name, "wpa_supplicant\n") ||
			 !strcmp(name, "hostapd\n"));
		fclose(comm);
		if (!found)
			continue;
		snprintf(path, sizeof(path), "/proc/%s/ns/net", entry->d_name);
		found = stat(path, &peer_namespace) == 0 &&
			own_namespace.st_dev == peer_namespace.st_dev &&
			own_namespace.st_ino == peer_namespace.st_ino;
		if (found)
			break;
	}
	closedir(processes);
	return found;
}

static enum fplinux_ui_wifi_result
check_interface(struct fplinux_session_wifi_station *station)
{
	const char *ifname = station->ifname;
	DBusMessage *request =
		new_call(station, WPA_ROOT, WPA_SERVICE, "GetInterface");
	DBusError error = DBUS_ERROR_INIT;
	DBusMessage *reply;
	enum fplinux_ui_wifi_result result = FPLINUX_UI_WIFI_FAILED;

	if (!request)
		return result;
	if (!dbus_message_append_args(request, DBUS_TYPE_STRING, &ifname,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(request);
		return result;
	}
	reply = call(station, request, &error);
	if (reply) {
		dbus_message_unref(reply);
		result = FPLINUX_UI_WIFI_BUSY;
	} else if (dbus_error_has_name(&error,
				       WPA_SERVICE ".InterfaceUnknown")) {
		result = FPLINUX_UI_WIFI_APPLIED;
	}
	dbus_error_free(&error);
	return result;
}

static enum fplinux_ui_wifi_result
create_interface(struct fplinux_session_wifi_station *station)
{
	DBusMessage *request =
		new_call(station, WPA_ROOT, WPA_SERVICE, "CreateInterface");
	DBusError error = DBUS_ERROR_INIT;
	DBusMessageIter args, dict;
	DBusMessage *reply;
	const char *path;
	enum fplinux_ui_wifi_result result = FPLINUX_UI_WIFI_FAILED;

	if (!request)
		return result;
	dbus_message_iter_init_append(request, &args);
	if (!open_dictionary(&args, &dict) ||
	    !append_text(&dict, "Ifname", station->ifname) ||
	    !append_text(&dict, "Driver", "nl80211") ||
	    !append_text(&dict, "ConfigFile", station->working_path) ||
	    !dbus_message_iter_close_container(&args, &dict)) {
		dbus_message_unref(request);
		return result;
	}
	reply = call(station, request, &error);
	if (reply) {
		if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH,
					  &path, DBUS_TYPE_INVALID) &&
		    copy_string(station->interface, sizeof(station->interface),
				path))
			result = FPLINUX_UI_WIFI_APPLIED;
		dbus_message_unref(reply);
	} else if (dbus_error_has_name(&error,
				       WPA_SERVICE ".InterfaceExists")) {
		result = FPLINUX_UI_WIFI_BUSY;
	}
	dbus_error_free(&error);
	return result;
}

enum fplinux_ui_wifi_result fplinux_session_wifi_station_acquire(
	struct fplinux_session_wifi_station *station)
{
	enum fplinux_ui_wifi_result result = FPLINUX_UI_WIFI_FAILED;
	DBusError error = DBUS_ERROR_INIT;
	char match[WPA_PATH_BYTES * 2 + 64];

	if (owns_interface(station))
		return FPLINUX_UI_WIFI_APPLIED;
	station->next_poll_ms = monotonic_ms() + IDLE_POLL_MS;
	station->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, NULL);
	if (!station->bus)
		return result;
	dbus_connection_set_exit_on_disconnect(station->bus, FALSE);
	if (!get_supplicant_owner(station))
		goto failed;
	result = check_interface(station);
	if (result != FPLINUX_UI_WIFI_APPLIED)
		goto failed;
	if (has_manual_manager(station)) {
		result = FPLINUX_UI_WIFI_BUSY;
		goto failed;
	}
	if (!prepare_working_config(station)) {
		result = FPLINUX_UI_WIFI_PROFILE_FAILED;
		goto failed;
	}
	result = create_interface(station);
	if (result != FPLINUX_UI_WIFI_APPLIED)
		goto failed;
	snprintf(match, sizeof(match), "type='signal',sender='%s',path='%s'",
		 station->owner, station->interface);
	dbus_bus_add_match(station->bus, match, &error);
	if (dbus_error_is_set(&error)) {
		dbus_error_free(&error);
		result = FPLINUX_UI_WIFI_FAILED;
		goto failed;
	}
	if (!load_profiles(station)) {
		result = FPLINUX_UI_WIFI_PROFILE_FAILED;
		goto failed;
	}
	present_profiles(station);
	station->next_poll_ms = 0;
	return result;

failed:
	release_interface(station);
	return result;
}

static bool keep_profile(const struct fplinux_session_wifi_station *station,
			 uint32_t profile_id, uint32_t add_id,
			 uint32_t remove_id)
{
	return profile_id && profile_id != remove_id &&
	       (profile_id == add_id || find_profile(station, profile_id));
}

/*
 * SaveConfig serializes the entire interface. Publish only the requested set
 * of profiles and leave each disabled on disk until the user selects it.
 */
static bool
copy_saved_profiles(const struct fplinux_session_wifi_station *station,
		    FILE *input, FILE *output, uint32_t add_id,
		    uint32_t remove_id)
{
	static const char network_begin[] = "network={\n";
	static const char network_disabled[] = "\tdisabled=1\n";
	char line[WIFI_NETWORK_BLOCK_BYTES];
	char block[WIFI_NETWORK_BLOCK_BYTES];
	const size_t expected = station->profile_count + !!add_id - !!remove_id;
	size_t length = 0, written = 0;
	uint32_t profile_id = 0;
	bool update_config = false;
	bool ok = false;

	while (fgets(line, sizeof(line), input)) {
		size_t bytes = strlen(line);

		if (!bytes || line[bytes - 1] != '\n')
			goto done;
		if (!length && !strcmp(line, network_begin)) {
			memcpy(block, network_begin, sizeof(network_begin) - 1);
			length = sizeof(network_begin) - 1;
			memcpy(block + length, network_disabled,
			       sizeof(network_disabled) - 1);
			length += sizeof(network_disabled) - 1;
			profile_id = 0;
			continue;
		}
		if (!length) {
			update_config |= !strcmp(line, "update_config=1\n");
			if (fputs(line, output) < 0)
				goto done;
			continue;
		}
		if (!strncmp(line, "\tid_str=", sizeof("\tid_str=") - 1))
			profile_id = config_line_tag(line, bytes);
		if (!strncmp(line, "\tdisabled=", sizeof("\tdisabled=") - 1))
			continue;
		if (bytes > sizeof(block) - length)
			goto done;
		memcpy(block + length, line, bytes);
		length += bytes;
		if (!strcmp(line, "}\n")) {
			if (keep_profile(station, profile_id, add_id,
					 remove_id)) {
				if (fwrite(block, 1, length, output) != length)
					goto done;
				++written;
			}
			length = 0;
		}
	}
	ok = !ferror(input) && !length && update_config && written == expected;
done:
	explicit_bzero(line, sizeof(line));
	explicit_bzero(block, sizeof(block));
	return ok;
}

static bool save_profiles(struct fplinux_session_wifi_station *station,
			  uint32_t add_id, uint32_t remove_id)
{
	char temporary[PATH_MAX];
	FILE *input, *output;
	int fd;
	bool ok;

	fd = open(station->working_tmp,
		  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	ok = fchmod(fd, 0600) == 0;
	close(fd);
	if (!ok || !path_call(station, station->interface, WPA_INTERFACE,
			      "SaveConfig", NULL))
		return false;
	if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX",
		     station->config_path) >= (int)sizeof(temporary))
		return false;
	input = fopen(station->working_path, "re");
	if (!input)
		return false;
	fd = mkostemp(temporary, O_CLOEXEC);
	output = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (output) {
		ok = fchmod(fd, 0600) == 0 &&
		     copy_saved_profiles(station, input, output, add_id,
					 remove_id) &&
		     fflush(output) == 0 && fsync(fd) == 0;
		ok = fclose(output) == 0 && ok;
		ok = ok && rename(temporary, station->config_path) == 0;
	} else {
		if (fd >= 0)
			close(fd);
		ok = false;
	}
	fclose(input);
	if (fd >= 0 && !ok)
		unlink(temporary);
	return ok;
}

static bool has_string(DBusMessageIter dict, const char *key, const char *text)
{
	DBusMessageIter field, array;

	if (!get_field(dict, key, DBUS_TYPE_ARRAY, &field) ||
	    dbus_message_iter_get_element_type(&field) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_recurse(&field, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
		const char *value;

		dbus_message_iter_get_basic(&array, &value);
		if (!strcmp(value, text))
			return true;
		dbus_message_iter_next(&array);
	}
	return false;
}

static bool has_enterprise(DBusMessageIter security)
{
	return has_string(security, "KeyMgmt", "wpa-eap") ||
	       has_string(security, "KeyMgmt", "wpa-ft-eap") ||
	       has_string(security, "KeyMgmt", "wpa-eap-sha256") ||
	       has_string(security, "KeyMgmt", "wpa-eap-suite-b") ||
	       has_string(security, "KeyMgmt", "wpa-eap-suite-b-192");
}

static enum fplinux_ui_wifi_security classify_bss(DBusMessageIter properties)
{
	DBusMessageIter rsn, wpa;
	dbus_bool_t privacy;
	bool has_rsn = get_field(properties, "RSN", DBUS_TYPE_ARRAY, &rsn);
	bool has_wpa = get_field(properties, "WPA", DBUS_TYPE_ARRAY, &wpa);

	if (has_rsn && has_string(rsn, "KeyMgmt", "wpa-psk") &&
	    has_string(rsn, "Pairwise", "ccmp") &&
	    !strcmp(get_text(rsn, "Group"), "ccmp"))
		return FPLINUX_UI_WIFI_WPA2;
	if (has_rsn && (has_string(rsn, "KeyMgmt", "sae") ||
			has_string(rsn, "KeyMgmt", "sae-ext-key") ||
			has_string(rsn, "KeyMgmt", "ft-sae") ||
			has_string(rsn, "KeyMgmt", "ft-sae-ext-key")))
		return FPLINUX_UI_WIFI_WPA3;
	if ((has_rsn && has_enterprise(rsn)) ||
	    (has_wpa && has_enterprise(wpa)))
		return FPLINUX_UI_WIFI_ENTERPRISE;
	if (get_basic(properties, "Privacy", DBUS_TYPE_BOOLEAN, &privacy) &&
	    !privacy && (!has_rsn || !get_text(rsn, "Group")[0]) &&
	    (!has_wpa || !get_text(wpa, "Group")[0]))
		return FPLINUX_UI_WIFI_OPEN;
	return FPLINUX_UI_WIFI_UNSUPPORTED;
}

static bool read_bss(struct fplinux_session_wifi_station *station,
		     const char *path, struct wifi_bss *bss)
{
	DBusMessage *reply = get_properties(station, path, WPA_BSS, NULL);
	DBusMessageIter properties;
	size_t bssid_bytes;
	bool ok = false;

	if (!reply)
		return false;
	if (dbus_message_has_signature(reply, "a{sv}") &&
	    dbus_message_iter_init(reply, &properties) &&
	    !strcmp(get_text(properties, "Mode"), "infrastructure") &&
	    get_bytes(properties, "SSID", bss->network.ssid.bytes,
		      sizeof(bss->network.ssid.bytes),
		      &bss->network.ssid.length) &&
	    get_bytes(properties, "BSSID", bss->bssid, sizeof(bss->bssid),
		      &bssid_bytes) &&
	    bssid_bytes == sizeof(bss->bssid) &&
	    get_basic(properties, "Signal", DBUS_TYPE_INT16, &bss->signal)) {
		bss->network.security = classify_bss(properties);
		ok = true;
	}
	dbus_message_unref(reply);
	return ok;
}

static void collect_bss(struct fplinux_session_wifi_station *station,
			struct wifi_bss *bss)
{
	for (size_t i = 0; i < station->scanned_count; ++i) {
		struct wifi_bss *row = &station->scanned[i];

		if (same_network(&row->network, &bss->network)) {
			if (bss->signal > row->signal) {
				bss->network.id = row->network.id;
				*row = *bss;
			}
			return;
		}
	}
	if (station->scanned_count == FPLINUX_UI_WIFI_ROW_COUNT)
		return;
	for (size_t i = 0; i < station->state.available_count; ++i) {
		const struct wifi_network *previous =
			&station->available[i].network;

		if (same_network(previous, &bss->network)) {
			bss->network.id = previous->id;
			break;
		}
	}
	if (!bss->network.id)
		bss->network.id = new_network_id();
	station->scanned[station->scanned_count++] = *bss;
}

static void finish_request(struct fplinux_session_wifi_station *station,
			   enum fplinux_ui_wifi_result result)
{
	station->ready_serial = station->pending_serial;
	station->ready_result = result;
	station->pending_serial = 0;
}

static void finish_scan(struct fplinux_session_wifi_station *station,
			bool success)
{
	if (station->scan_reply) {
		dbus_message_unref(station->scan_reply);
		station->scan_reply = NULL;
	}
	station->scan_waiting = false;
	if (success) {
		station->state.scan = FPLINUX_UI_WIFI_SCAN_READY;
		station->state.available_count = station->scanned_count;
		memcpy(station->available, station->scanned,
		       station->scanned_count * sizeof(station->available[0]));
		present_scan(station);
	} else {
		station->state.scan = FPLINUX_UI_WIFI_SCAN_FAILED;
	}
	finish_request(station, success ? FPLINUX_UI_WIFI_APPLIED :
					  FPLINUX_UI_WIFI_FAILED);
}

static void scan_finished(struct fplinux_session_wifi_station *station,
			  DBusMessage *signal)
{
	DBusMessageIter value, array;
	dbus_bool_t success;

	station->scan_waiting = false;
	if (!dbus_message_get_args(signal, NULL, DBUS_TYPE_BOOLEAN, &success,
				   DBUS_TYPE_INVALID) ||
	    !success) {
		finish_scan(station, false);
		return;
	}
	station->scan_reply = get_properties(station, station->interface,
					     WPA_INTERFACE, "BSSs");
	if (!station->scan_reply ||
	    !dbus_message_has_signature(station->scan_reply, "v") ||
	    !dbus_message_iter_init(station->scan_reply, &value)) {
		finish_scan(station, false);
		return;
	}
	dbus_message_iter_recurse(&value, &array);
	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY ||
	    dbus_message_iter_get_element_type(&array) !=
		    DBUS_TYPE_OBJECT_PATH) {
		finish_scan(station, false);
		return;
	}
	dbus_message_iter_recurse(&array, &station->scan_paths);
}

static bool get_scanning(struct fplinux_session_wifi_station *station,
			 dbus_bool_t *scanning, uint32_t *reply_serial)
{
	DBusMessage *reply = get_properties(station, station->interface,
					    WPA_INTERFACE, "Scanning");
	DBusMessageIter args, value;
	bool valid = false;

	if (!reply)
		return false;
	*reply_serial = dbus_message_get_serial(reply);
	if (dbus_message_has_signature(reply, "v") &&
	    dbus_message_iter_init(reply, &args)) {
		dbus_message_iter_recurse(&args, &value);
		valid = dbus_message_iter_get_arg_type(&value) ==
			DBUS_TYPE_BOOLEAN;
		if (valid)
			dbus_message_iter_get_basic(&value, scanning);
	}
	dbus_message_unref(reply);
	return valid;
}

static bool start_scan(struct fplinux_session_wifi_station *station,
		       uint32_t serial, enum fplinux_ui_wifi_result *result)
{
	dbus_bool_t scanning;
	uint32_t reply_serial;

	*result = FPLINUX_UI_WIFI_FAILED;
	if (!get_scanning(station, &scanning, &reply_serial))
		return true;
	/* An enabled interface can already have a scan in progress. */
	if (!scanning) {
		DBusMessage *request = new_call(station, station->interface,
						WPA_INTERFACE, "Scan");
		DBusMessage *reply;
		DBusMessageIter args, dict;
		dbus_bool_t allow_roam = FALSE;

		if (!request)
			return true;
		dbus_message_iter_init_append(request, &args);
		if (!open_dictionary(&args, &dict) ||
		    !append_text(&dict, "Type", "active") ||
		    !append_value(&dict, "AllowRoam", "b", DBUS_TYPE_BOOLEAN,
				  &allow_roam, 0) ||
		    !dbus_message_iter_close_container(&args, &dict)) {
			dbus_message_unref(request);
			return true;
		}
		reply = call(station, request, NULL);
		if (!reply) {
			station->state.scan = FPLINUX_UI_WIFI_SCAN_FAILED;
			return true;
		}
		reply_serial = dbus_message_get_serial(reply);
		dbus_message_unref(reply);
	}
	station->scan_reply_serial = reply_serial;
	station->scan_waiting = true;
	station->scanned_count = 0;
	station->state.scan = FPLINUX_UI_WIFI_SCANNING;
	station->pending_serial = serial;
	station->deadline_ms = monotonic_ms() + SCAN_TIMEOUT_MS;
	return false;
}

static void collect_scan(struct fplinux_session_wifi_station *station)
{
	uint64_t budget_ms = monotonic_ms() + 50;

	for (size_t i = 0; i < SCAN_RESULTS_PER_UPDATE; ++i) {
		struct wifi_bss bss = { 0 };
		const char *path;

		if (dbus_message_iter_get_arg_type(&station->scan_paths) !=
		    DBUS_TYPE_OBJECT_PATH) {
			finish_scan(station, true);
			break;
		}
		dbus_message_iter_get_basic(&station->scan_paths, &path);
		dbus_message_iter_next(&station->scan_paths);
		if (read_bss(station, path, &bss))
			collect_bss(station, &bss);
		if (monotonic_ms() >= budget_ms)
			break;
	}
}

static bool valid_password(const char *key)
{
	size_t bytes = strnlen(key, FPLINUX_UI_WIFI_KEY_BYTES + 1);

	if (bytes < 8 || bytes > FPLINUX_UI_WIFI_KEY_BYTES)
		return false;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char character = key[i];

		if (bytes == FPLINUX_UI_WIFI_KEY_BYTES ?
			    hex_value(character) < 0 :
			    character < 0x20 || character > 0x7e)
			return false;
	}
	return true;
}

static bool add_network(struct fplinux_session_wifi_station *station,
			struct wifi_selection *selection, const char *key)
{
	DBusMessage *request = new_call(station, station->interface,
					WPA_INTERFACE, "AddNetwork");
	char tag[sizeof(WIFI_PROFILE_TAG) + 8];
	const struct wifi_ssid *ssid = &selection->network.ssid;
	dbus_uint32_t disabled = 1, hidden = selection->hidden;
	unsigned char psk[FPLINUX_UI_WIFI_KEY_BYTES / 2];
	DBusMessageIter args, dict;
	DBusMessage *reply;
	const char *path;
	bool ok;

	if (!request)
		return false;
	snprintf(tag, sizeof(tag), WIFI_PROFILE_TAG "%08x",
		 selection->network.id);
	dbus_message_iter_init_append(request, &args);
	ok = open_dictionary(&args, &dict) &&
	     append_value(&dict, "ssid", "ay", DBUS_TYPE_ARRAY, ssid->bytes,
			  ssid->length) &&
	     append_text(&dict, "id_str", tag) &&
	     append_text(&dict, "key_mgmt", "WPA-PSK") &&
	     append_text(&dict, "proto", "RSN") &&
	     append_text(&dict, "pairwise", "CCMP") &&
	     append_text(&dict, "group", "CCMP") &&
	     append_value(&dict, "disabled", "u", DBUS_TYPE_UINT32, &disabled,
			  0) &&
	     append_value(&dict, "scan_ssid", "u", DBUS_TYPE_UINT32, &hidden,
			  0);
	if (strlen(key) == FPLINUX_UI_WIFI_KEY_BYTES) {
		ok = ok && decode_hex(key, psk, sizeof(psk)) &&
		     append_value(&dict, "psk", "ay", DBUS_TYPE_ARRAY, psk,
				  sizeof(psk));
		explicit_bzero(psk, sizeof(psk));
	} else {
		ok = ok && append_text(&dict, "psk", key);
	}
	if (!ok || !dbus_message_iter_close_container(&args, &dict)) {
		dbus_message_unref(request);
		return false;
	}
	reply = call(station, request, NULL);
	if (!reply)
		return false;
	ok = dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				   DBUS_TYPE_INVALID) &&
	     copy_string(selection->path, sizeof(selection->path), path);
	dbus_message_unref(reply);
	return ok;
}

static bool select_network(struct fplinux_session_wifi_station *station,
			   const char *path)
{
	DBusMessage *request = new_call(station, station->interface,
					WPA_INTERFACE, "SelectNetwork");
	DBusMessage *reply;

	if (!request)
		return false;
	if (!dbus_message_append_args(request, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(request);
		return false;
	}
	reply = call(station, request, NULL);
	if (!reply)
		return false;
	station->select_reply_serial = dbus_message_get_serial(reply);
	dbus_message_unref(reply);
	return true;
}

/* A saved path can be reused only after the old selection is gone. */
static bool selection_is_clear(struct fplinux_session_wifi_station *station,
			       const char *path)
{
	DBusMessage *reply = get_properties(station, station->interface,
					    WPA_INTERFACE, NULL);
	DBusMessageIter properties;
	const char *current;
	bool clear = false;

	if (!reply)
		return false;
	if (dbus_message_has_signature(reply, "a{sv}") &&
	    dbus_message_iter_init(reply, &properties) &&
	    get_basic(properties, "CurrentNetwork", DBUS_TYPE_OBJECT_PATH,
		      &current)) {
		const char *state = get_text(properties, "State");

		clear = strcmp(current, path) ||
			!strcmp(state, "disconnected") ||
			!strcmp(state, "inactive") ||
			!strcmp(state, "interface_disabled");
	}
	dbus_message_unref(reply);
	return clear;
}

static const struct wifi_network *
find_available(const struct fplinux_session_wifi_station *station, uint32_t id)
{
	for (size_t i = 0; i < station->state.available_count; ++i)
		if (station->available[i].network.id == id)
			return &station->available[i].network;
	return NULL;
}

static bool get_selection(struct fplinux_session_wifi_station *station,
			  const struct fplinux_ui_wifi_request *request,
			  struct wifi_selection *selection)
{
	const struct wifi_network *network;
	const struct wifi_profile *profile;

	switch (request->action) {
	case FPLINUX_UI_WIFI_CONNECT_HIDDEN:
		selection->network.ssid.length =
			strnlen(request->ssid, sizeof(request->ssid));
		if (!selection->network.ssid.length ||
		    selection->network.ssid.length > FPLINUX_UI_WIFI_SSID_BYTES)
			return false;
		memcpy(selection->network.ssid.bytes, request->ssid,
		       selection->network.ssid.length);
		selection->network.security = request->security;
		selection->hidden = true;
		break;
	case FPLINUX_UI_WIFI_CONNECT_SAVED:
		profile = find_profile(station, request->target);
		if (!profile)
			return false;
		selection->network = profile->network;
		selection->hidden = profile->hidden;
		if (!request->key[0]) {
			selection->profile_id = profile->network.id;
			strcpy(selection->path, profile->path);
		}
		break;
	case FPLINUX_UI_WIFI_CONNECT:
		network = find_available(station, request->target);
		if (!network)
			return false;
		selection->network = *network;
		break;
	default:
		return false;
	}
	if (!selection->profile_id) {
		selection->network.id = new_network_id();
		selection->save = request->save;
	}
	return selection->network.security == FPLINUX_UI_WIFI_WPA2;
}

static bool start_connection(struct fplinux_session_wifi_station *station,
			     const struct fplinux_ui_wifi_request *request,
			     uint32_t serial,
			     enum fplinux_ui_wifi_result *result)
{
	struct wifi_selection selection = { 0 };
	*result = FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE;
	if (!get_selection(station, request, &selection))
		return true;
	*result = FPLINUX_UI_WIFI_FAILED;
	if (!selection.profile_id &&
	    (!valid_password(request->key) ||
	     !add_network(station, &selection, request->key)))
		return true;
	if (!release_selection(station)) {
		release_interface(station);
		return true;
	}
	if (selection.profile_id &&
	    !selection_is_clear(station, selection.path)) {
		*result = FPLINUX_UI_WIFI_BUSY;
		return true;
	}
	station->selected = selection;
	if (!select_network(station, selection.path)) {
		if (!release_selection(station))
			release_interface(station);
		return true;
	}
	present_network(&station->state.current, &selection.network,
			selection.profile_id);
	station->state.link = FPLINUX_UI_WIFI_CONNECTING;
	station->pending_serial = serial;
	station->deadline_ms = monotonic_ms() + CONNECT_TIMEOUT_MS;
	station->next_poll_ms = 0;
	return false;
}

static void erase_profile(struct fplinux_session_wifi_station *station,
			  const struct wifi_profile *profile)
{
	size_t index = profile - station->profiles;
	size_t remaining = station->profile_count - index - 1;

	memmove(&station->profiles[index], &station->profiles[index + 1],
		remaining * sizeof(*profile));
	--station->profile_count;
}

static enum fplinux_ui_wifi_result
forget_profile(struct fplinux_session_wifi_station *station, uint32_t id)
{
	const struct wifi_profile *profile = find_profile(station, id);
	char path[WPA_PATH_BYTES];

	if (!profile)
		return FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE;
	if (!save_profiles(station, 0, id))
		return FPLINUX_UI_WIFI_PROFILE_FAILED;
	strcpy(path, profile->path);
	erase_profile(station, profile);
	present_profiles(station);
	if (station->selected.profile_id == id && !release_selection(station)) {
		release_interface(station);
		return FPLINUX_UI_WIFI_FAILED;
	}
	if (strcmp(station->retired_path, path))
		remove_network(station, path);
	return FPLINUX_UI_WIFI_APPLIED;
}

static bool commit_selection(struct fplinux_session_wifi_station *station)
{
	struct wifi_selection *selected = &station->selected;
	uint32_t replace = matching_profile(station, &selected->network);
	const struct wifi_profile *old = find_profile(station, replace);
	struct wifi_profile *profile;

	if (station->profile_count == FPLINUX_UI_WIFI_ROW_COUNT && !replace)
		return false;
	if (!save_profiles(station, selected->network.id, replace))
		return false;
	if (old) {
		remove_network(station, old->path);
		erase_profile(station, old);
	}
	profile = &station->profiles[station->profile_count++];
	*profile = (struct wifi_profile){
		.network = selected->network,
		.hidden = selected->hidden,
	};
	strcpy(profile->path, selected->path);
	selected->profile_id = profile->network.id;
	present_profiles(station);
	return true;
}

static void observe_association(struct fplinux_session_wifi_station *station,
				DBusMessageIter properties)
{
	struct wifi_selection *selected = &station->selected;
	const char *state = get_text(properties, "State");
	const char *current = "/";
	bool matching;

	if (!selected->path[0])
		return;
	get_basic(properties, "CurrentNetwork", DBUS_TYPE_OBJECT_PATH,
		  &current);
	matching = !strcmp(selected->path, current);
	if (matching && !strcmp(state, "completed")) {
		if (station->pending_serial &&
		    station->state.link == FPLINUX_UI_WIFI_CONNECTING)
			finish_request(station, FPLINUX_UI_WIFI_APPLIED);
		present_network(&station->state.current, &selected->network,
				selected->profile_id);
		station->state.link = FPLINUX_UI_WIFI_ADDRESSING;
		return;
	}
	if (station->state.link != FPLINUX_UI_WIFI_CONNECTING ||
	    !station->pending_serial) {
		selected->save = false;
		station->state.link = FPLINUX_UI_WIFI_DISCONNECTED;
		memset(&station->state.current, 0,
		       sizeof(station->state.current));
		return;
	}
	if (matching && (!strcmp(state, "4way_handshake") ||
			 !strcmp(state, "group_handshake")))
		selected->reached_handshake = true;
	if (matching &&
	    (!strcmp(state, "authenticating") ||
	     !strcmp(state, "associating") || !strcmp(state, "associated") ||
	     selected->reached_handshake))
		selected->reached_authentication = true;
}

static bool message_follows(DBusMessage *message, uint32_t serial)
{
	uint32_t delta = dbus_message_get_serial(message) - serial;

	return delta && delta < (UINT32_MAX / 2);
}

static void receive_signals(struct fplinux_session_wifi_station *station)
{
	DBusMessage *message;

	dbus_connection_read_write(station->bus, 0);
	for (size_t i = 0; i < 64; ++i) {
		message = dbus_connection_pop_message(station->bus);
		if (!message)
			break;
		if (!dbus_message_has_sender(message, station->owner) ||
		    !dbus_message_has_path(message, station->interface)) {
			dbus_message_unref(message);
			continue;
		}
		if (station->scan_waiting &&
		    dbus_message_is_signal(message, WPA_INTERFACE,
					   "ScanDone") &&
		    message_follows(message, station->scan_reply_serial))
			scan_finished(station, message);
		if (station->pending_serial &&
		    station->state.link == FPLINUX_UI_WIFI_CONNECTING &&
		    station->selected.reached_handshake &&
		    dbus_message_is_signal(message, WPA_INTERFACE,
					   "PskMismatch") &&
		    message_follows(message, station->select_reply_serial))
			station->selected.psk_mismatch = true;
		if (dbus_message_is_signal(message, WPA_PROPERTIES,
					   "PropertiesChanged"))
			station->next_poll_ms = 0;
		dbus_message_unref(message);
	}
}

static bool poll_interface(struct fplinux_session_wifi_station *station)
{
	DBusMessage *reply = get_properties(station, station->interface,
					    WPA_INTERFACE, NULL);
	DBusMessageIter properties;
	uint64_t interval = station->selected.path[0] ? LINK_POLL_MS :
							IDLE_POLL_MS;
	bool valid = reply && dbus_message_has_signature(reply, "a{sv}") &&
		     dbus_message_iter_init(reply, &properties) &&
		     get_text(properties, "State")[0];

	station->next_poll_ms = monotonic_ms() + interval;
	if (valid) {
		station->missing_updates = 0;
		observe_association(station, properties);
	} else if (++station->missing_updates >= 3) {
		if (station->pending_serial)
			finish_request(station,
				       FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
		release_interface(station);
	}
	if (reply)
		dbus_message_unref(reply);
	return valid;
}

enum fplinux_ui_wifi_result fplinux_session_wifi_station_commit(
	struct fplinux_session_wifi_station *station)
{
	if (station->state.link != FPLINUX_UI_WIFI_ADDRESSING)
		return FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE;
	if (!station->selected.save)
		return FPLINUX_UI_WIFI_APPLIED;
	if (!poll_interface(station) ||
	    station->state.link != FPLINUX_UI_WIFI_ADDRESSING) {
		station->selected.save = false;
		return FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE;
	}
	station->selected.save = false;
	if (!commit_selection(station))
		return FPLINUX_UI_WIFI_PROFILE_FAILED;
	station->state.current.profile_id = station->selected.profile_id;
	return FPLINUX_UI_WIFI_APPLIED;
}

static void fail_selection(struct fplinux_session_wifi_station *station,
			   enum fplinux_ui_wifi_result result)
{
	finish_request(station, result);
	if (!release_selection(station))
		release_interface(station);
}

void fplinux_session_wifi_station_update(
	struct fplinux_session_wifi_station *station)
{
	if (!owns_interface(station) &&
	    (monotonic_ms() < station->next_poll_ms ||
	     fplinux_session_wifi_station_acquire(station) !=
		     FPLINUX_UI_WIFI_APPLIED))
		return;
	receive_signals(station);
	if (dbus_connection_get_is_connected(station->bus) &&
	    monotonic_ms() >= station->next_poll_ms)
		poll_interface(station);
	if (!station->bus)
		return;
	if (!dbus_connection_get_is_connected(station->bus)) {
		if (station->pending_serial)
			finish_request(station,
				       FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
		release_interface(station);
		return;
	}
	if (station->scan_reply)
		collect_scan(station);
	if (!station->pending_serial)
		return;
	if (station->state.scan == FPLINUX_UI_WIFI_SCANNING &&
	    monotonic_ms() >= station->deadline_ms) {
		if (station->scan_waiting)
			path_call(station, station->interface, WPA_INTERFACE,
				  "AbortScan", NULL);
		finish_scan(station, false);
	} else if (station->state.link == FPLINUX_UI_WIFI_CONNECTING) {
		if (station->selected.psk_mismatch)
			fail_selection(station, FPLINUX_UI_WIFI_BAD_PASSWORD);
		else if (monotonic_ms() >= station->deadline_ms)
			fail_selection(
				station,
				station->selected.reached_authentication ?
					FPLINUX_UI_WIFI_AUTH_FAILED :
					FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
	}
}

static enum fplinux_ui_wifi_result
cancel_request(struct fplinux_session_wifi_station *station, uint32_t target)
{
	if (!target || station->pending_serial != target)
		return FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE;
	station->pending_serial = 0;
	if (station->state.scan == FPLINUX_UI_WIFI_SCANNING) {
		if (station->scan_waiting)
			path_call(station, station->interface, WPA_INTERFACE,
				  "AbortScan", NULL);
		finish_scan(station, false);
	} else if (!release_selection(station)) {
		release_interface(station);
		return FPLINUX_UI_WIFI_FAILED;
	}
	return FPLINUX_UI_WIFI_APPLIED;
}

bool fplinux_session_wifi_station_request(
	struct fplinux_session_wifi_station *station,
	const struct fplinux_ui_wifi_request *request, uint32_t serial,
	enum fplinux_ui_wifi_result *result)
{
	if (request->action == FPLINUX_UI_WIFI_CANCEL) {
		*result = cancel_request(station, request->target);
		return true;
	}
	if (station->pending_serial || station->ready_serial) {
		*result = FPLINUX_UI_WIFI_BUSY;
		return true;
	}
	*result = fplinux_session_wifi_station_acquire(station);
	if (*result != FPLINUX_UI_WIFI_APPLIED)
		return true;
	switch (request->action) {
	case FPLINUX_UI_WIFI_SCAN:
		return start_scan(station, serial, result);
	case FPLINUX_UI_WIFI_CONNECT:
	case FPLINUX_UI_WIFI_CONNECT_SAVED:
	case FPLINUX_UI_WIFI_CONNECT_HIDDEN:
		return start_connection(station, request, serial, result);
	case FPLINUX_UI_WIFI_DISCONNECT:
		if (!release_selection(station)) {
			release_interface(station);
			*result = FPLINUX_UI_WIFI_FAILED;
		}
		return true;
	case FPLINUX_UI_WIFI_FORGET:
		*result = forget_profile(station, request->target);
		return true;
	default:
		*result = FPLINUX_UI_WIFI_FAILED;
		return true;
	}
}

struct fplinux_session_wifi_station *
fplinux_session_wifi_station_create(const char *ifname, const char *config_path)
{
	struct fplinux_session_wifi_station *station =
		calloc(1, sizeof(*station));

	if (!station)
		return NULL;
	if (!config_path)
		config_path = WIFI_CONFIG_PATH;
	if (!ifname[0] || config_path[0] != '/' ||
	    !copy_string(station->ifname, sizeof(station->ifname), ifname) ||
	    !copy_string(station->config_path, sizeof(station->config_path),
			 config_path)) {
		free(station);
		return NULL;
	}
	station->state.power = FPLINUX_UI_WIFI_ON;
	return station;
}

void fplinux_session_wifi_station_destroy(
	struct fplinux_session_wifi_station *station)
{
	if (!station)
		return;
	release_interface(station);
	free(station);
}

bool fplinux_session_wifi_station_take_result(
	struct fplinux_session_wifi_station *station, uint32_t *serial,
	enum fplinux_ui_wifi_result *result)
{
	if (!station->ready_serial)
		return false;
	*serial = station->ready_serial;
	*result = station->ready_result;
	station->ready_serial = 0;
	return true;
}

const struct fplinux_ui_wifi_state *fplinux_session_wifi_station_state(
	const struct fplinux_session_wifi_station *station)
{
	return &station->state;
}
