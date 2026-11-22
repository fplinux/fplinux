/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-ui-session-internal.h"
#include "fplinux-utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
/* Linux permits at most 253 descriptors in one SCM_RIGHTS message. */
#define SESSION_MAX_FDS 253
#define VIBRATION_PULSE_MIN_MS 50
#define VIBRATION_PULSE_MAX_MS 3000
#define VIBRATION_PAUSE_MIN_MS 100
#define VIBRATION_PAUSE_MAX_MS 60000

int fplinux_ui_session_document_path_error(const char *path)
{
	char copy[FPLINUX_UI_SESSION_PATH_BYTES];
	char *save = NULL;
	char *component;
	int directory;
	int error = 0;
	const unsigned char *cursor, *end;
	uint32_t scalar;

	if (!path || path[0] != '/' || strlen(path) >= sizeof(copy))
		return EINVAL;
	/* JSON paths must remain valid text; reject before sending a packet. */
	cursor = (const unsigned char *)path;
	end = cursor + strlen(path);
	while (cursor != end)
		if (!fplinux_utf8_next(&cursor, end, &scalar))
			return EILSEQ;
	strcpy(copy, path);
	directory = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0)
		return errno;
	component = strtok_r(copy, "/", &save);
	if (!component)
		error = EISDIR;
	while (component && !error) {
		char *next = strtok_r(NULL, "/", &save);
		struct stat status;
		int fd;

		if (!strcmp(component, ".") || !strcmp(component, "..")) {
			error = EINVAL;
			break;
		}
		fd = openat(directory, component,
			    O_PATH | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			error = errno;
			break;
		}
		if (fstat(fd, &status) < 0)
			error = errno;
		else if (S_ISLNK(status.st_mode))
			error = ELOOP;
		else if (next && !S_ISDIR(status.st_mode))
			error = ENOTDIR;
		else if (!next && !S_ISREG(status.st_mode))
			error = S_ISDIR(status.st_mode) ? EISDIR : ENOTSUP;
		close(directory);
		directory = fd;
		component = next;
	}
	close(directory);
	return error;
}

static const char *const message_names[] = {
	[FPLINUX_UI_SESSION_HELLO] = "HELLO",
	[FPLINUX_UI_SESSION_READY] = "READY",
	[FPLINUX_UI_SESSION_LAUNCH] = "LAUNCH",
	[FPLINUX_UI_SESSION_TITLE] = "TITLE",
	[FPLINUX_UI_SESSION_SOFTKEYS] = "SOFTKEYS",
	[FPLINUX_UI_SESSION_HOME] = "HOME",
	[FPLINUX_UI_SESSION_EXIT] = "EXIT",
	[FPLINUX_UI_SESSION_SELECT] = "SELECT",
	[FPLINUX_UI_SESSION_CONFIG] = "CONFIG",
	[FPLINUX_UI_SESSION_ACTIVE] = "ACTIVE",
	[FPLINUX_UI_SESSION_CLOSE] = "CLOSE",
	[FPLINUX_UI_SESSION_ERROR] = "ERROR",
	[FPLINUX_UI_SESSION_VISIBILITY] = "VISIBILITY",
	[FPLINUX_UI_SESSION_KEEP_SCREEN_ON] = "KEEP_SCREEN_ON",
	[FPLINUX_UI_SESSION_SETTING] = "SETTING",
	[FPLINUX_UI_SESSION_DISMISS] = "DISMISS",
	[FPLINUX_UI_SESSION_MODE] = "MODE",
	[FPLINUX_UI_SESSION_ACTIVE_ACK] = "ACTIVE_ACK",
	[FPLINUX_UI_SESSION_CANCEL] = "CANCEL",
	[FPLINUX_UI_SESSION_BUSY] = "BUSY",
	[FPLINUX_UI_SESSION_LIFECYCLE] = "LIFECYCLE",
	[FPLINUX_UI_SESSION_TASKS] = "TASKS",
	[FPLINUX_UI_SESSION_ACTIVATE] = "ACTIVATE",
	[FPLINUX_UI_SESSION_CLOSE_TASK] = "CLOSE_TASK",
	[FPLINUX_UI_SESSION_REDRAW_ACK] = "REDRAW_ACK",
	[FPLINUX_UI_SESSION_CLOCK] = "clock",
	[FPLINUX_UI_SESSION_VIBRATE] = "VIBRATE",
	[FPLINUX_UI_SESSION_PREPARE_CLOSE] = "PREPARE_CLOSE",
	[FPLINUX_UI_SESSION_CLOSE_REPLY] = "CLOSE_REPLY",
	[FPLINUX_UI_SESSION_CLOSE_ACK] = "CLOSE_ACK",
	[FPLINUX_UI_SESSION_ACTIVATION] = "ACTIVATION",
	[FPLINUX_UI_SESSION_ACTIVATION_ACK] = "ACTIVATION_ACK",
	[FPLINUX_UI_SESSION_OPEN_FILE] = "OPEN_FILE",
	[FPLINUX_UI_SESSION_DOCUMENT_CHOICES] = "DOCUMENT_CHOICES",
	[FPLINUX_UI_SESSION_DOCUMENT_CHOICE] = "DOCUMENT_CHOICE",
	[FPLINUX_UI_SESSION_DOCUMENT] = "DOCUMENT",
	[FPLINUX_UI_SESSION_DOCUMENT_RESULT] = "DOCUMENT_RESULT",
	[FPLINUX_UI_SESSION_STORAGE_REQUEST] = "STORAGE_REQUEST",
	[FPLINUX_UI_SESSION_STORAGE_RESULT] = "STORAGE_RESULT",
	[FPLINUX_UI_SESSION_SYSTEM_LEVELS] = "SYSTEM_LEVELS",
	[FPLINUX_UI_SESSION_LIGHT_EFFECT] = "LIGHT_EFFECT",
	[FPLINUX_UI_SESSION_WIFI_REQUEST] = "WIFI_REQUEST",
	[FPLINUX_UI_SESSION_WIFI_STATE] = "WIFI_STATE",
	[FPLINUX_UI_SESSION_WIFI_RESULT] = "WIFI_RESULT",
	[FPLINUX_UI_SESSION_WIFI_STATUS] = "WIFI_STATUS",
	[FPLINUX_UI_SESSION_BLUETOOTH_READY] = "BLUETOOTH_READY",
	[FPLINUX_UI_SESSION_BLUETOOTH_ACTION] = "BLUETOOTH_ACTION",
	[FPLINUX_UI_SESSION_BLUETOOTH_SEND] = "BLUETOOTH_SEND",
};

static const char *const level_names[] = {
	[FPLINUX_SYSTEM_LEVEL_MEDIA] = "media",
	[FPLINUX_SYSTEM_LEVEL_ALARM] = "alarm",
	[FPLINUX_SYSTEM_LEVEL_BRIGHTNESS] = "brightness",
};

static const char *const level_status_names[] = {
	[FPLINUX_SYSTEM_LEVEL_UNKNOWN] = "unknown",
	[FPLINUX_SYSTEM_LEVEL_READY] = "ready",
	[FPLINUX_SYSTEM_LEVEL_UNAVAILABLE] = "unavailable",
};

static const char *const volume_operation_names[] = {
	[FPLINUX_VOLUME_REFRESH] = "refresh",
	[FPLINUX_VOLUME_CONNECT] = "connect",
	[FPLINUX_VOLUME_EJECT] = "eject",
};

static const char *const document_status_names[] = {
	[FPLINUX_UI_DOCUMENT_ACCEPTED] = "accepted",
	[FPLINUX_UI_DOCUMENT_WAIT] = "wait",
	[FPLINUX_UI_DOCUMENT_CANCELLED] = "cancelled",
	[FPLINUX_UI_DOCUMENT_ERROR] = "error",
	[FPLINUX_UI_DOCUMENT_RESTART_REQUIRED] = "restart-required",
};

static const char *const activation_names[] = {
	[FPLINUX_UI_SESSION_ACTIVATION_OPEN] = "open",
	[FPLINUX_UI_SESSION_ACTIVATION_RESUME] = "resume",
};

static const char *const lifecycle_names[] = {
	[FPLINUX_UI_SESSION_FOREGROUND] = "foreground",
	[FPLINUX_UI_SESSION_BACKGROUND] = "background",
	[FPLINUX_UI_SESSION_CLOSING] = "closing",
};

static const char *const playback_names[] = {
	[FPLINUX_UI_SESSION_PLAYBACK_NONE] = "none",
	[FPLINUX_UI_SESSION_PLAYBACK_PAUSED] = "paused",
	[FPLINUX_UI_SESSION_PLAYBACK_HELD] = "held",
	[FPLINUX_UI_SESSION_PLAYBACK_UNAVAILABLE] = "unavailable",
	[FPLINUX_UI_SESSION_PLAYBACK_PLAYING] = "playing",
};

static const char *const mode_names[] = {
	[FPLINUX_UI_SESSION_LAUNCHER] = "launcher",
	[FPLINUX_UI_SESSION_INTEGRATED] = "integrated",
	[FPLINUX_UI_SESSION_FULLSCREEN] = "fullscreen",
};

static const char *const display_names[] = {
	[FPLINUX_UI_SESSION_DISPLAY_ON] = "on",
	[FPLINUX_UI_SESSION_DISPLAY_DIM] = "dim",
	[FPLINUX_UI_SESSION_DISPLAY_OFF] = "off",
};

static const char *const theme_names[] = {
	[FPLINUX_UI_SESSION_THEME_LIGHT] = "light",
	[FPLINUX_UI_SESSION_THEME_DARK] = "dark",
};

static const char *const storage_names[] = {
	[FPLINUX_STORAGE_UNPUBLISHED] = "unpublished",
	[FPLINUX_STORAGE_PUBLISHED_UNSYNCED] = "published-unsynced",
	[FPLINUX_STORAGE_DURABLE] = "durable",
};

static const char *const route_names[] = {
	[FPLINUX_AUDIO_ROUTE_AUTO] = "AUTO",
	[FPLINUX_AUDIO_ROUTE_SPEAKER] = "SPEAKER",
	[FPLINUX_AUDIO_ROUTE_HEADPHONES] = "HEADPHONES",
};

static const char *const setting_names[] = {
	[FPLINUX_UI_SETTING_BRIGHTNESS] = "brightness",
	[FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW] = "brightness_preview",
	[FPLINUX_UI_SETTING_VOLUME] = "volume",
	[FPLINUX_UI_SETTING_VOLUME_PREVIEW] = "volume_preview",
	[FPLINUX_UI_SETTING_VOLUME_STEP] = "volume_step",
	[FPLINUX_UI_SETTING_VOLUME_COMMIT] = "volume_commit",
	[FPLINUX_UI_SETTING_MEDIA_VOLUME_STEP] = "media_volume_step",
	[FPLINUX_UI_SETTING_ALARM_VOLUME] = "alarm_volume",
	[FPLINUX_UI_SETTING_ALARM_VOLUME_PREVIEW] = "alarm_volume_preview",
	[FPLINUX_UI_SETTING_ALARM_VOLUME_COMMIT] = "alarm_volume_commit",
	[FPLINUX_UI_SETTING_AUDIO_OUTPUT] = "audio_output",
	[FPLINUX_UI_SETTING_AUDIO_OUTPUTS] = "audio_outputs",
	[FPLINUX_UI_SETTING_AUDIO_OUTPUT_ACTIVE] = "audio_output_active",
	[FPLINUX_UI_SETTING_SCREEN_OFF] = "screen_off_seconds",
	[FPLINUX_UI_SETTING_AUTO_LOCK] = "auto_lock_seconds",
	[FPLINUX_UI_SETTING_LOCALE] = "locale",
	[FPLINUX_UI_SETTING_INPUT_LOCALE] = "input_locale",
	[FPLINUX_UI_SETTING_TIME] = "time",
	[FPLINUX_UI_SETTING_THEME] = "theme",
	[FPLINUX_UI_SETTING_WIFI] = "wifi",
	[FPLINUX_UI_SETTING_MIDI_SOUNDFONT] = "midi_soundfont",
	[FPLINUX_UI_SETTING_UNKNOWN] = "unknown",
};

static const char *const setting_operation_names[] = {
	[FPLINUX_UI_SETTING_GET] = "get",
	[FPLINUX_UI_SETTING_SET] = "set",
	[FPLINUX_UI_SETTING_PREVIEW] = "preview",
	[FPLINUX_UI_SETTING_CANCEL] = "cancel",
	[FPLINUX_UI_SETTING_STEP] = "step",
	[FPLINUX_UI_SETTING_COMMIT] = "commit",
	[FPLINUX_UI_SETTING_RESULT] = "result",
	[FPLINUX_UI_SETTING_INVALID] = "invalid",
};

static const char *const launcher_view_names[] = {
	[FPLINUX_UI_LAUNCHER_VIEW_HOME] = "home",
	[FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE] = "catalogue",
	[FPLINUX_UI_LAUNCHER_VIEW_TASKS] = "tasks",
	[FPLINUX_UI_LAUNCHER_VIEW_QUICK_SETTINGS] = "quick-settings",
};

static const char *const launcher_catalogue_names[] = {
	[FPLINUX_UI_LAUNCHER_CATALOGUE_GRID] = "grid",
	[FPLINUX_UI_LAUNCHER_CATALOGUE_LIST] = "list",
};

static const char *const close_response_names[] = {
	[FPLINUX_UI_SESSION_CLOSE_READY] = "ready",
	[FPLINUX_UI_SESSION_CLOSE_CANCEL] = "cancel",
	[FPLINUX_UI_SESSION_CLOSE_WAIT] = "wait",
};

/* Every field a packet may carry; anything else makes it invalid. */
static const char *const field_names[] = {
	"type",
	"mode",
	"app_id",
	"active_id",
	"selected_id",
	"title",
	"left",
	"centre",
	"right",
	"reason",
	"width",
	"height",
	"display",
	"locked",
	"visible",
	"enabled",
	"tasks_view",
	"background_playing",
	"locale",
	"input_locale",
	"name",
	"value",
	"setting",
	"launcher_view",
	"launcher_catalogue",
	"battery",
	"charging",
	"notice",
	"screen_width",
	"screen_height",
	"theme",
	"serial",
	"lifecycle",
	"tasks",
	"token",
	"clock",
	"clock_presentation",
	"audio",
	"background_audio",
	"vibration",
	"pattern",
	"repeat",
	"applied",
	"storage_state",
	"storage_error",
	"activation",
	"path",
	"document_status",
	"document_error",
	"document_choices",
	"volume_operation",
	"volume_id",
	"volume_error",
	"home_persistent",
	"volumes",
	"levels",
	"light_effects",
	"light_effect",
	"wifi",
	"bluetooth_prompt",
};

static void add_string(struct json_object *object, const char *name,
		       const char *value)
{
	/* An absent string field reads as empty; keep packets small. */
	if (value[0])
		json_object_object_add(object, name,
				       json_object_new_string(value));
}

static bool get_string(struct json_object *object, const char *name,
		       char *value, size_t capacity)
{
	struct json_object *field;
	const char *text;
	size_t bytes;

	if (!json_object_object_get_ex(object, name, &field))
		return true;
	if (!json_object_is_type(field, json_type_string))
		return false;
	text = json_object_get_string(field);
	bytes = json_object_get_string_len(field);
	if (bytes >= capacity || strlen(text) != bytes)
		return false;
	/*
	 * No field carries control characters. json-c escapes each one to six
	 * bytes, so accepting them would let a client inflate the ACTIVE message
	 * that the manager relays to the launcher beyond one packet.
	 */
	for (size_t i = 0; i < bytes; ++i)
		if ((unsigned char)text[i] < 0x20 || text[i] == 0x7f)
			return false;
	memcpy(value, text, bytes + 1);
	return true;
}

static bool clock_data_valid(const char *text)
{
	size_t bytes = strnlen(text, FPLINUX_UI_SESSION_CLOCK_BYTES);

	if (bytes == FPLINUX_UI_SESSION_CLOCK_BYTES)
		return false;
	for (size_t i = 0; i < bytes; ++i)
		if ((unsigned char)text[i] < 0x20 || text[i] == 0x7f)
			return false;
	return true;
}

static const char *json_space(const char *cursor, const char *end)
{
	while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
				*cursor == '\n' || *cursor == '\r'))
		++cursor;
	return cursor;
}

static const char *json_string_end(const char *cursor, const char *end)
{
	if (cursor == end || *cursor++ != '"')
		return NULL;
	while (cursor < end) {
		char byte = *cursor++;

		if (byte == '"')
			return cursor;
		if (byte == '\\' && cursor < end)
			++cursor;
	}
	return NULL;
}

/* Find the end of a value in an already parsed packet. Only the clock owner
 * interprets the original domain tokens, including duplicate keys.
 */
static const char *json_value_end(const char *cursor, const char *end)
{
	size_t depth = 0;

	if (cursor == end)
		return NULL;
	if (*cursor == '"')
		return json_string_end(cursor, end);
	if (*cursor != '{' && *cursor != '[') {
		const char *start = cursor;

		while (cursor < end && *cursor != ',' && *cursor != '}' &&
		       json_space(cursor, end) == cursor)
			++cursor;
		return cursor == start ? NULL : cursor;
	}
	while (cursor < end) {
		char byte = *cursor;

		if (byte == '"') {
			cursor = json_string_end(cursor, end);
			if (!cursor)
				return NULL;
			continue;
		}
		++cursor;
		if (byte == '{' || byte == '[')
			++depth;
		else if ((byte == '}' || byte == ']') && !--depth)
			return cursor;
	}
	return NULL;
}

static int clock_key(const char *key, size_t bytes)
{
	struct json_tokener *tokener;
	struct json_object *name;
	int result = -1;

	if (bytes == sizeof("\"clock\"") - 1 &&
	    !memcmp(key, "\"clock\"", bytes))
		return 1;
	if (!memchr(key, '\\', bytes))
		return 0;
	tokener = json_tokener_new();
	if (!tokener)
		return -1;
	name = json_tokener_parse_ex(tokener, key, (int)bytes);
	if (json_tokener_get_error(tokener) == json_tokener_success &&
	    json_object_is_type(name, json_type_string)) {
		const char *text = json_object_get_string(name);
		size_t length = (size_t)json_object_get_string_len(name);

		if (!memchr(text, '\0', length))
			result = length == 5 && !memcmp(text, "clock", length);
	}
	json_object_put(name);
	json_tokener_free(tokener);
	return result;
}

static bool copy_clock_object(const char *packet, size_t bytes, char *data)
{
	const char *end = packet + bytes;
	const char *cursor = json_space(packet, end);
	bool found = false;

	if (cursor == end || *cursor++ != '{')
		return false;
	while ((cursor = json_space(cursor, end)) < end && *cursor != '}') {
		const char *key = cursor;
		const char *key_end = json_string_end(key, end);
		const char *value, *value_end;
		int is_clock;

		if (!key_end)
			return false;
		is_clock = clock_key(key, (size_t)(key_end - key));
		cursor = json_space(key_end, end);
		if (is_clock < 0 || cursor == end || *cursor++ != ':')
			return false;
		value = json_space(cursor, end);
		value_end = json_value_end(value, end);
		if (!value_end)
			return false;
		if (is_clock) {
			size_t length = value_end - value;

			if (found || *value != '{' ||
			    length >= FPLINUX_UI_SESSION_CLOCK_BYTES)
				return false;
			memcpy(data, value, length);
			data[length] = '\0';
			found = true;
		}
		cursor = json_space(value_end, end);
		if (cursor == end)
			return false;
		if (*cursor == '}')
			return found && json_space(cursor + 1, end) == end;
		if (*cursor++ != ',')
			return false;
	}
	return false;
}

static bool get_clock(struct json_object *object, unsigned int type,
		      const char *packet, size_t bytes, char *data)
{
	struct json_object *field;

	if (!json_object_object_get_ex(object, "clock", &field))
		return true;
	return (type == FPLINUX_UI_SESSION_CLOCK ||
		type == FPLINUX_UI_SESSION_ACTIVE) &&
	       json_object_is_type(field, json_type_object) &&
	       copy_clock_object(packet, bytes, data);
}

static bool add_clock(struct json_object *object, const char *data)
{
	struct json_tokener *tokener;
	struct json_object *field;
	size_t bytes = strlen(data);
	bool valid;

	if (!bytes)
		return true;
	tokener = json_tokener_new();
	if (!tokener)
		return false;
	json_tokener_set_flags(tokener, JSON_TOKENER_VALIDATE_UTF8);
	field = json_tokener_parse_ex(tokener, data, (int)bytes);
	valid = json_tokener_get_error(tokener) == json_tokener_success &&
		json_tokener_get_parse_end(tokener) == bytes &&
		json_object_is_type(field, json_type_object);
	json_tokener_free(tokener);
	if (!valid) {
		json_object_put(field);
		errno = EINVAL;
		return false;
	}
	/* Preserve the checked object's raw tokens until this packet is sent. */
	json_object_set_serializer(field, json_object_userdata_to_json_string,
				   (void *)data, NULL);
	if (json_object_object_add(object, "clock", field) < 0) {
		json_object_put(field);
		return false;
	}
	return true;
}

static bool get_int(struct json_object *object, const char *name, int *value)
{
	struct json_object *field;
	int64_t number;

	if (!json_object_object_get_ex(object, name, &field))
		return true;
	if (!json_object_is_type(field, json_type_int))
		return false;
	number = json_object_get_int64(field);
	if (number < INT32_MIN || number > INT32_MAX)
		return false;
	*value = number;
	return true;
}

static bool get_bool(struct json_object *object, const char *name, bool *value)
{
	struct json_object *field;

	if (!json_object_object_get_ex(object, name, &field))
		return true;
	if (!json_object_is_type(field, json_type_boolean))
		return false;
	*value = json_object_get_boolean(field);
	return true;
}

static bool wifi_text_valid(const char *text, size_t capacity)
{
	const unsigned char *cursor = (const unsigned char *)text;
	size_t length = strnlen(text, capacity);
	const unsigned char *end = cursor + length;
	uint32_t scalar;

	if (length == capacity)
		return false;
	while (cursor != end)
		if (!fplinux_utf8_next(&cursor, end, &scalar) ||
		    scalar < 0x20 || scalar == 0x7f)
			return false;
	return true;
}

static bool wifi_network_valid(const struct fplinux_ui_wifi_network *network)
{
	return network->id && network->name[0] &&
	       wifi_text_valid(network->name, sizeof(network->name)) &&
	       (unsigned int)network->security <= FPLINUX_UI_WIFI_UNSUPPORTED;
}

static bool wifi_rows_valid(const struct fplinux_ui_wifi_network *rows,
			    size_t count, bool saved)
{
	if (count > FPLINUX_UI_WIFI_ROW_COUNT)
		return false;
	for (size_t i = 0; i < count; ++i) {
		if (!wifi_network_valid(&rows[i]) ||
		    (saved && rows[i].profile_id != rows[i].id))
			return false;
		for (size_t j = 0; j < i; ++j)
			if (rows[i].id == rows[j].id)
				return false;
	}
	return true;
}

static bool wifi_fields_valid(const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_wifi_state *state = &message->wifi_state;
	const struct fplinux_ui_wifi_request *request = &message->wifi_request;
	const struct fplinux_ui_wifi_status *status = &message->wifi_status;

	switch (message->type) {
	case FPLINUX_UI_SESSION_WIFI_STATUS:
		return !message->serial &&
		       (unsigned int)status->power <= FPLINUX_UI_WIFI_ON &&
		       (unsigned int)status->link <=
			       FPLINUX_UI_WIFI_NO_ADDRESS &&
		       wifi_text_valid(status->name, sizeof(status->name)) &&
		       (status->link == FPLINUX_UI_WIFI_DISCONNECTED ?
				!status->name[0] :
				status->power == FPLINUX_UI_WIFI_ON &&
					status->name[0]);
	case FPLINUX_UI_SESSION_WIFI_STATE:
		return !message->serial &&
		       (unsigned int)state->power <= FPLINUX_UI_WIFI_ON &&
		       (unsigned int)state->scan <=
			       FPLINUX_UI_WIFI_SCAN_FAILED &&
		       (unsigned int)state->link <=
			       FPLINUX_UI_WIFI_NO_ADDRESS &&
		       (state->link == FPLINUX_UI_WIFI_DISCONNECTED ||
			(state->power == FPLINUX_UI_WIFI_ON &&
			 wifi_network_valid(&state->current))) &&
		       (state->link != FPLINUX_UI_WIFI_CONNECTED ||
			state->address[0]) &&
		       wifi_text_valid(state->address,
				       sizeof(state->address)) &&
		       wifi_rows_valid(state->available, state->available_count,
				       false) &&
		       wifi_rows_valid(state->saved, state->saved_count, true);
	case FPLINUX_UI_SESSION_WIFI_REQUEST:
		if (!message->serial ||
		    !wifi_text_valid(request->ssid, sizeof(request->ssid)) ||
		    !wifi_text_valid(request->key, sizeof(request->key)) ||
		    (unsigned int)request->security > FPLINUX_UI_WIFI_WPA2 ||
		    (request->action != FPLINUX_UI_WIFI_CONNECT_HIDDEN &&
		     request->ssid[0]) ||
		    (request->action != FPLINUX_UI_WIFI_CONNECT_HIDDEN &&
		     request->action != FPLINUX_UI_WIFI_CONNECT &&
		     request->action != FPLINUX_UI_WIFI_CONNECT_SAVED &&
		     (request->key[0] || request->save)))
			return false;
		switch (request->action) {
		case FPLINUX_UI_WIFI_CONNECT_HIDDEN:
			return !request->target && request->ssid[0];
		case FPLINUX_UI_WIFI_CONNECT:
		case FPLINUX_UI_WIFI_CONNECT_SAVED:
		case FPLINUX_UI_WIFI_CANCEL:
		case FPLINUX_UI_WIFI_FORGET:
			return request->target != 0;
		case FPLINUX_UI_WIFI_SET_POWER:
		case FPLINUX_UI_WIFI_SCAN:
		case FPLINUX_UI_WIFI_DISCONNECT:
		case FPLINUX_UI_WIFI_RETRY_ADDRESS:
			return !request->target;
		}
		return false;
	case FPLINUX_UI_SESSION_WIFI_RESULT:
		return message->serial && (unsigned int)message->wifi_result <=
						  FPLINUX_UI_WIFI_FAILED;
	default:
		return true;
	}
}

static void add_wifi_u32(struct json_object *object, const char *name,
			 uint32_t value)
{
	json_object_object_add(object, name, json_object_new_int64(value));
}

static bool get_wifi_u32(struct json_object *object, const char *name,
			 uint32_t *value)
{
	struct json_object *field;
	int64_t number;

	if (!json_object_object_get_ex(object, name, &field) ||
	    !json_object_is_type(field, json_type_int))
		return false;
	number = json_object_get_int64(field);
	if (number < 0 || number > UINT32_MAX)
		return false;
	*value = (uint32_t)number;
	return true;
}

static struct json_object *
wifi_network_object(const struct fplinux_ui_wifi_network *network)
{
	struct json_object *entry = json_object_new_object();

	add_wifi_u32(entry, "id", network->id);
	add_wifi_u32(entry, "profile", network->profile_id);
	add_wifi_u32(entry, "security", network->security);
	add_string(entry, "name", network->name);
	return entry;
}

static bool get_wifi_network(struct json_object *object,
			     struct fplinux_ui_wifi_network *network)
{
	uint32_t security;

	if (!json_object_is_type(object, json_type_object) ||
	    !get_wifi_u32(object, "id", &network->id) ||
	    !get_wifi_u32(object, "profile", &network->profile_id) ||
	    !get_wifi_u32(object, "security", &security) ||
	    security > FPLINUX_UI_WIFI_UNSUPPORTED ||
	    !get_string(object, "name", network->name, sizeof(network->name)))
		return false;
	network->security = security;
	return true;
}

static struct json_object *
wifi_rows_object(const struct fplinux_ui_wifi_network *rows, size_t count)
{
	struct json_object *array = json_object_new_array();

	for (size_t i = 0; i < count; ++i)
		json_object_array_add(array, wifi_network_object(&rows[i]));
	return array;
}

static bool get_wifi_rows(struct json_object *object, const char *name,
			  struct fplinux_ui_wifi_network *rows, size_t *count)
{
	struct json_object *array;

	if (!json_object_object_get_ex(object, name, &array) ||
	    !json_object_is_type(array, json_type_array))
		return false;
	*count = json_object_array_length(array);
	if (*count > FPLINUX_UI_WIFI_ROW_COUNT)
		return false;
	for (size_t i = 0; i < *count; ++i)
		if (!get_wifi_network(json_object_array_get_idx(array, i),
				      &rows[i]))
			return false;
	return true;
}

static void add_wifi_fields(struct json_object *object,
			    const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_wifi_state *state = &message->wifi_state;
	const struct fplinux_ui_wifi_request *request = &message->wifi_request;
	struct json_object *wifi;

	if (message->type != FPLINUX_UI_SESSION_WIFI_STATE &&
	    message->type != FPLINUX_UI_SESSION_WIFI_REQUEST &&
	    message->type != FPLINUX_UI_SESSION_WIFI_RESULT &&
	    message->type != FPLINUX_UI_SESSION_WIFI_STATUS)
		return;
	wifi = json_object_new_object();
	json_object_object_add(object, "wifi", wifi);
	if (message->serial)
		add_wifi_u32(object, "serial", message->serial);
	switch (message->type) {
	case FPLINUX_UI_SESSION_WIFI_STATUS:
		add_wifi_u32(wifi, "power", message->wifi_status.power);
		add_wifi_u32(wifi, "link", message->wifi_status.link);
		json_object_object_add(
			wifi, "name",
			json_object_new_string(message->wifi_status.name));
		break;
	case FPLINUX_UI_SESSION_WIFI_STATE:
		add_wifi_u32(wifi, "power", state->power);
		add_wifi_u32(wifi, "scan", state->scan);
		add_wifi_u32(wifi, "link", state->link);
		json_object_object_add(
			wifi, "open_supported",
			json_object_new_boolean(state->open_supported));
		if (state->link != FPLINUX_UI_WIFI_DISCONNECTED)
			json_object_object_add(
				wifi, "current",
				wifi_network_object(&state->current));
		add_string(wifi, "address", state->address);
		json_object_object_add(
			wifi, "available",
			wifi_rows_object(state->available,
					 state->available_count));
		json_object_object_add(wifi, "saved",
				       wifi_rows_object(state->saved,
							state->saved_count));
		break;
	case FPLINUX_UI_SESSION_WIFI_REQUEST:
		add_wifi_u32(wifi, "action", request->action);
		add_wifi_u32(wifi, "target", request->target);
		add_wifi_u32(wifi, "security", request->security);
		json_object_object_add(
			wifi, "enabled",
			json_object_new_boolean(request->enabled));
		json_object_object_add(wifi, "save",
				       json_object_new_boolean(request->save));
		add_string(wifi, "ssid", request->ssid);
		add_string(wifi, "key", request->key);
		break;
	case FPLINUX_UI_SESSION_WIFI_RESULT:
		add_wifi_u32(wifi, "result", message->wifi_result);
		break;
	default:
		break;
	}
}

static bool get_wifi_state(struct json_object *wifi,
			   struct fplinux_ui_wifi_state *state)
{
	struct json_object *current;
	uint32_t power, scan, link;

	if (!get_wifi_u32(wifi, "power", &power) ||
	    !get_wifi_u32(wifi, "scan", &scan) ||
	    !get_wifi_u32(wifi, "link", &link) || power > FPLINUX_UI_WIFI_ON ||
	    scan > FPLINUX_UI_WIFI_SCAN_FAILED ||
	    link > FPLINUX_UI_WIFI_NO_ADDRESS)
		return false;
	state->power = power;
	state->scan = scan;
	state->link = link;
	if (link != FPLINUX_UI_WIFI_DISCONNECTED &&
	    (!json_object_object_get_ex(wifi, "current", &current) ||
	     !get_wifi_network(current, &state->current)))
		return false;
	return get_bool(wifi, "open_supported", &state->open_supported) &&
	       get_string(wifi, "address", state->address,
			  sizeof(state->address)) &&
	       get_wifi_rows(wifi, "available", state->available,
			     &state->available_count) &&
	       get_wifi_rows(wifi, "saved", state->saved, &state->saved_count);
}

static bool get_wifi_fields(struct json_object *object,
			    struct fplinux_ui_session_message *message)
{
	struct fplinux_ui_wifi_request *request = &message->wifi_request;
	struct json_object *wifi = NULL;
	uint32_t action, security, result, power, link;

	json_object_object_get_ex(object, "wifi", &wifi);
	switch (message->type) {
	case FPLINUX_UI_SESSION_WIFI_STATUS:
		if (!json_object_is_type(wifi, json_type_object) ||
		    json_object_object_length(wifi) != 3 ||
		    !get_wifi_u32(wifi, "power", &power) ||
		    !get_wifi_u32(wifi, "link", &link) ||
		    power > FPLINUX_UI_WIFI_ON ||
		    link > FPLINUX_UI_WIFI_NO_ADDRESS)
			return false;
		message->wifi_status.power = power;
		message->wifi_status.link = link;
		return get_string(wifi, "name", message->wifi_status.name,
				  sizeof(message->wifi_status.name));
	case FPLINUX_UI_SESSION_WIFI_STATE:
		return json_object_is_type(wifi, json_type_object) &&
		       get_wifi_state(wifi, &message->wifi_state);
	case FPLINUX_UI_SESSION_WIFI_REQUEST:
		if (!json_object_is_type(wifi, json_type_object) ||
		    !get_wifi_u32(wifi, "action", &action) ||
		    !get_wifi_u32(wifi, "security", &security) ||
		    action > FPLINUX_UI_WIFI_FORGET ||
		    security > FPLINUX_UI_WIFI_WPA2)
			return false;
		request->action = action;
		request->security = security;
		return get_wifi_u32(wifi, "target", &request->target) &&
		       get_bool(wifi, "enabled", &request->enabled) &&
		       get_bool(wifi, "save", &request->save) &&
		       get_string(wifi, "ssid", request->ssid,
				  sizeof(request->ssid)) &&
		       get_string(wifi, "key", request->key,
				  sizeof(request->key));
	case FPLINUX_UI_SESSION_WIFI_RESULT:
		if (!json_object_is_type(wifi, json_type_object) ||
		    !get_wifi_u32(wifi, "result", &result) ||
		    result > FPLINUX_UI_WIFI_FAILED)
			return false;
		message->wifi_result = result;
		return true;
	default:
		return !wifi;
	}
}

static bool
clock_presentation_valid(const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_session_clock_presentation *clock =
		&message->clock_presentation;
	size_t length = strnlen(clock->alarm_label, sizeof(clock->alarm_label));

	if (length == sizeof(clock->alarm_label) || clock->alarm_utc < 0 ||
	    (clock->alarm_utc && clock->snooze_deadline_ms) ||
	    (clock->timer_deadline_ms && clock->timer_remaining_ms) ||
	    (length && !clock->alarm_utc && !clock->snooze_deadline_ms))
		return false;
	for (size_t i = 0; i < length; ++i)
		if ((unsigned char)clock->alarm_label[i] < 0x20 ||
		    clock->alarm_label[i] == 0x7f)
			return false;
	return message->type == FPLINUX_UI_SESSION_ACTIVE ||
	       (!clock->analog && !clock->alarm_utc &&
		!clock->snooze_deadline_ms && !length &&
		!clock->timer_deadline_ms && !clock->timer_remaining_ms &&
		!clock->missed_total);
}

/* Decimal strings retain the entire uint64 range without JSON saturation. */
static void add_clock_number(struct json_object *object, const char *name,
			     uint64_t value)
{
	char text[21];

	if (!value)
		return;
	snprintf(text, sizeof(text), "%" PRIu64, value);
	add_string(object, name, text);
}

static bool get_clock_number(struct json_object *object, const char *name,
			     uint64_t *value)
{
	struct json_object *field;
	char text[21], *end;
	unsigned long long parsed;

	if (!json_object_object_get_ex(object, name, &field))
		return true;
	if (!get_string(object, name, text, sizeof(text)) || text[0] < '0' ||
	    text[0] > '9')
		return false;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end)
		return false;
	*value = parsed;
	return true;
}

static void add_clock_presentation(
	struct json_object *object,
	const struct fplinux_ui_session_clock_presentation *clock)
{
	struct json_object *state = json_object_new_object();

	json_object_object_add(state, "analog",
			       json_object_new_boolean(clock->analog));
	add_clock_number(state, "alarm_utc", clock->alarm_utc);
	add_clock_number(state, "snooze_deadline_ms",
			 clock->snooze_deadline_ms);
	add_string(state, "alarm_label", clock->alarm_label);
	add_clock_number(state, "timer_deadline_ms", clock->timer_deadline_ms);
	add_clock_number(state, "timer_remaining_ms",
			 clock->timer_remaining_ms);
	add_clock_number(state, "missed_total", clock->missed_total);
	json_object_object_add(object, "clock_presentation", state);
}

static bool get_clock_presentation(struct json_object *object,
				   unsigned int type,
				   struct fplinux_ui_session_message *message)
{
	struct fplinux_ui_session_clock_presentation *clock =
		&message->clock_presentation;
	struct json_object *state;
	uint64_t alarm_utc = 0;

	if (!json_object_object_get_ex(object, "clock_presentation", &state))
		return true;
	if (type != FPLINUX_UI_SESSION_ACTIVE ||
	    !json_object_is_type(state, json_type_object))
		return false;
	json_object_object_foreach(state, name, field)
	{
		(void)field;
		if (strcmp(name, "analog") && strcmp(name, "alarm_utc") &&
		    strcmp(name, "snooze_deadline_ms") &&
		    strcmp(name, "alarm_label") &&
		    strcmp(name, "timer_deadline_ms") &&
		    strcmp(name, "timer_remaining_ms") &&
		    strcmp(name, "missed_total"))
			return false;
	}
	if (!get_bool(state, "analog", &clock->analog) ||
	    !get_clock_number(state, "alarm_utc", &alarm_utc) ||
	    alarm_utc > INT64_MAX ||
	    !get_clock_number(state, "snooze_deadline_ms",
			      &clock->snooze_deadline_ms) ||
	    !get_string(state, "alarm_label", clock->alarm_label,
			sizeof(clock->alarm_label)) ||
	    !get_clock_number(state, "timer_deadline_ms",
			      &clock->timer_deadline_ms) ||
	    !get_clock_number(state, "timer_remaining_ms",
			      &clock->timer_remaining_ms) ||
	    !get_clock_number(state, "missed_total", &clock->missed_total))
		return false;
	clock->alarm_utc = alarm_utc;
	return true;
}

/* Finds name in a table; an empty name selects fallback. */
static bool get_choice(struct json_object *object, const char *field_name,
		       const char *const *names, unsigned int count,
		       unsigned int fallback, unsigned int *choice)
{
	struct json_object *field;
	const char *name;
	unsigned int i;

	if (!json_object_object_get_ex(object, field_name, &field)) {
		*choice = fallback;
		return true;
	}
	if (!json_object_is_type(field, json_type_string))
		return false;
	name = json_object_get_string(field);
	if (strlen(name) != (size_t)json_object_get_string_len(field))
		return false;
	if (!name[0]) {
		*choice = fallback;
		return true;
	}
	for (i = 0; i < count; ++i)
		if (names[i] && !strcmp(name, names[i])) {
			*choice = i;
			return true;
		}
	return false;
}

static bool system_level_valid(const struct fplinux_system_level_state *level)
{
	if ((unsigned int)level->availability >=
		    ARRAY_SIZE(level_status_names) ||
	    level->minimum >= level->maximum || level->maximum > UINT16_MAX)
		return false;
	if (level->availability == FPLINUX_SYSTEM_LEVEL_READY)
		return level->value >= level->minimum &&
		       level->value <= level->maximum;
	return !level->value &&
	       (level->availability != FPLINUX_SYSTEM_LEVEL_UNAVAILABLE ||
		!level->pending);
}

static bool
system_levels_valid(const struct fplinux_ui_session_message *message)
{
	for (size_t i = 0; i < ARRAY_SIZE(level_names); ++i) {
		const struct fplinux_system_level_state *level =
			&message->system_levels.level[i];

		if (message->type == FPLINUX_UI_SESSION_SYSTEM_LEVELS) {
			if (!system_level_valid(level))
				return false;
		} else if (level->availability || level->minimum ||
			   level->maximum || level->value || level->pending) {
			return false;
		}
	}
	return true;
}

static void add_system_levels(struct json_object *object,
			      const struct fplinux_system_levels *levels)
{
	struct json_object *state = json_object_new_object();

	for (size_t i = 0; i < ARRAY_SIZE(level_names); ++i) {
		const struct fplinux_system_level_state *level =
			&levels->level[i];
		struct json_object *entry = json_object_new_object();

		add_string(entry, "status",
			   level_status_names[level->availability]);
		json_object_object_add(entry, "minimum",
				       json_object_new_int(level->minimum));
		json_object_object_add(entry, "maximum",
				       json_object_new_int(level->maximum));
		json_object_object_add(entry, "value",
				       json_object_new_int(level->value));
		json_object_object_add(entry, "pending",
				       json_object_new_boolean(level->pending));
		json_object_object_add(state, level_names[i], entry);
	}
	json_object_object_add(object, "levels", state);
}

static bool get_system_levels(struct json_object *object, unsigned int type,
			      struct fplinux_system_levels *levels)
{
	static const char *const fields[] = {
		"status", "minimum", "maximum", "value", "pending",
	};
	struct json_object *state, *field;
	bool present = json_object_object_get_ex(object, "levels", &state);

	if (type != FPLINUX_UI_SESSION_SYSTEM_LEVELS)
		return !present;
	if (!present || !json_object_is_type(state, json_type_object) ||
	    json_object_object_length(state) != (int)ARRAY_SIZE(level_names))
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(level_names); ++i) {
		struct fplinux_system_level_state *level = &levels->level[i];
		struct json_object *entry;
		unsigned int availability;
		int minimum = 0, maximum = 0, value = 0;

		if (!json_object_object_get_ex(state, level_names[i], &entry) ||
		    !json_object_is_type(entry, json_type_object) ||
		    json_object_object_length(entry) != (int)ARRAY_SIZE(fields))
			return false;
		for (size_t j = 0; j < ARRAY_SIZE(fields); ++j)
			if (!json_object_object_get_ex(entry, fields[j],
						       &field))
				return false;
		if (!get_choice(entry, "status", level_status_names,
				ARRAY_SIZE(level_status_names),
				ARRAY_SIZE(level_status_names),
				&availability) ||
		    availability >= ARRAY_SIZE(level_status_names) ||
		    !get_int(entry, "minimum", &minimum) || minimum < 0 ||
		    !get_int(entry, "maximum", &maximum) || maximum < 0 ||
		    !get_int(entry, "value", &value) || value < 0 ||
		    !get_bool(entry, "pending", &level->pending))
			return false;
		level->availability = availability;
		level->minimum = minimum;
		level->maximum = maximum;
		level->value = value;
		if (!system_level_valid(level))
			return false;
	}
	return true;
}

static bool has_only_known_fields(struct json_object *object)
{
	struct json_object_iterator field = json_object_iter_begin(object);
	struct json_object_iterator end = json_object_iter_end(object);

	for (; !json_object_iter_equal(&field, &end);
	     json_object_iter_next(&field)) {
		const char *name = json_object_iter_peek_name(&field);
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(field_names); ++i)
			if (!strcmp(name, field_names[i]))
				break;
		if (i == ARRAY_SIZE(field_names))
			return false;
	}
	return true;
}

static bool get_lifecycle(struct json_object *object,
			  enum fplinux_ui_session_lifecycle *lifecycle)
{
	unsigned int choice;

	if (!get_choice(object, "lifecycle", lifecycle_names,
			ARRAY_SIZE(lifecycle_names),
			ARRAY_SIZE(lifecycle_names), &choice) ||
	    choice >= ARRAY_SIZE(lifecycle_names))
		return false;
	*lifecycle = choice;
	return true;
}

static bool task_id_valid(const char *id)
{
	size_t bytes = strnlen(id, FPLINUX_UI_SESSION_ID_BYTES);

	if (!bytes || bytes == FPLINUX_UI_SESSION_ID_BYTES)
		return false;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char c = (unsigned char)id[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
		      c == '-'))
			return false;
	}
	return true;
}

static bool task_valid(const struct fplinux_ui_session_task *task)
{
	size_t bytes = strnlen(task->title, sizeof(task->title));

	if (!task_id_valid(task->app_id) || !bytes ||
	    bytes == sizeof(task->title) || !task->token ||
	    (unsigned int)task->lifecycle >= ARRAY_SIZE(lifecycle_names) ||
	    (unsigned int)task->playback >= ARRAY_SIZE(playback_names))
		return false;
	for (size_t i = 0; i < bytes; ++i)
		if ((unsigned char)task->title[i] < 0x20 ||
		    task->title[i] == 0x7f)
			return false;
	return true;
}

static bool tasks_valid(const struct fplinux_ui_session_message *message)
{
	if (message->task_count > FPLINUX_UI_SESSION_TASK_MAX ||
	    (!message->has_tasks && message->task_count))
		return false;
	for (unsigned int i = 0; i < message->task_count; ++i) {
		if (!task_valid(&message->tasks[i]))
			return false;
		for (unsigned int j = 0; j < i; ++j)
			if (!strcmp(message->tasks[i].app_id,
				    message->tasks[j].app_id) ||
			    message->tasks[i].token == message->tasks[j].token)
				return false;
	}
	return true;
}

static bool get_token(struct json_object *object, uint32_t *token)
{
	struct json_object *field;
	int64_t value;

	if (!json_object_object_get_ex(object, "token", &field) ||
	    !json_object_is_type(field, json_type_int))
		return false;
	value = json_object_get_int64(field);
	if (value <= 0 || value > UINT32_MAX)
		return false;
	*token = (uint32_t)value;
	return true;
}

static bool get_tasks(struct json_object *object,
		      struct fplinux_ui_session_message *message)
{
	struct json_object *array;
	size_t count;

	if (!json_object_object_get_ex(object, "tasks", &array))
		return true;
	if (!json_object_is_type(array, json_type_array))
		return false;
	count = json_object_array_length(array);
	if (count > FPLINUX_UI_SESSION_TASK_MAX)
		return false;
	message->has_tasks = true;
	message->task_count = count;
	for (size_t i = 0; i < count; ++i) {
		struct json_object *entry = json_object_array_get_idx(array, i);
		struct fplinux_ui_session_task *task = &message->tasks[i];
		struct json_object *field;
		unsigned int playback;
		bool has_playback;

		if (!json_object_is_type(entry, json_type_object))
			return false;
		has_playback =
			json_object_object_get_ex(entry, "playback", &field);
		if (json_object_object_length(entry) != 4 + has_playback ||
		    !get_string(entry, "app_id", task->app_id,
				sizeof(task->app_id)) ||
		    !get_string(entry, "title", task->title,
				sizeof(task->title)) ||
		    !get_lifecycle(entry, &task->lifecycle) ||
		    !get_choice(entry, "playback", playback_names,
				ARRAY_SIZE(playback_names),
				FPLINUX_UI_SESSION_PLAYBACK_NONE, &playback) ||
		    !get_token(entry, &task->token))
			return false;
		task->playback = playback;
	}
	return tasks_valid(message);
}

static bool get_task_fields(struct json_object *object, unsigned int type,
			    struct fplinux_ui_session_message *message)
{
	struct json_object *field;

	if (json_object_object_get_ex(object, "lifecycle", &field) &&
	    type != FPLINUX_UI_SESSION_LIFECYCLE)
		return false;
	if (json_object_object_get_ex(object, "activation", &field) &&
	    type != FPLINUX_UI_SESSION_ACTIVATION)
		return false;
	if (type == FPLINUX_UI_SESSION_ACTIVATION) {
		unsigned int activation = ARRAY_SIZE(activation_names);

		if (!get_choice(object, "activation", activation_names,
				ARRAY_SIZE(activation_names),
				ARRAY_SIZE(activation_names), &activation) ||
		    activation >= ARRAY_SIZE(activation_names))
			return false;
		message->activation = activation;
	}
	if (json_object_object_get_ex(object, "tasks", &field) &&
	    type != FPLINUX_UI_SESSION_TASKS)
		return false;
	if (json_object_object_get_ex(object, "token", &field) &&
	    type != FPLINUX_UI_SESSION_ACTIVATE &&
	    type != FPLINUX_UI_SESSION_CLOSE_TASK &&
	    type != FPLINUX_UI_SESSION_ACTIVE)
		return false;
	if (type == FPLINUX_UI_SESSION_LIFECYCLE)
		return get_lifecycle(object, &message->lifecycle);
	if (type == FPLINUX_UI_SESSION_TASKS)
		return get_tasks(object, message);
	if (type == FPLINUX_UI_SESSION_ACTIVATE ||
	    type == FPLINUX_UI_SESSION_CLOSE_TASK)
		return task_id_valid(message->app_id) &&
		       get_token(object, &message->token);
	if (type == FPLINUX_UI_SESSION_ACTIVE &&
	    json_object_object_get_ex(object, "token", &field))
		return task_id_valid(message->active_id) &&
		       get_token(object, &message->token);
	return true;
}

enum setting_payload {
	SETTING_NO_PAYLOAD,
	SETTING_LEVEL,
	SETTING_STEP,
	SETTING_SECONDS,
	SETTING_ROUTE,
	SETTING_ROUTES,
	SETTING_LOCALE,
	SETTING_TIME,
	SETTING_THEME,
	SETTING_ENABLED,
	SETTING_SOUNDFONT,
};

static enum setting_payload
setting_payload(const struct fplinux_ui_setting *setting)
{
	switch (setting->operation) {
	case FPLINUX_UI_SETTING_GET:
	case FPLINUX_UI_SETTING_CANCEL:
	case FPLINUX_UI_SETTING_COMMIT:
	case FPLINUX_UI_SETTING_INVALID:
		return SETTING_NO_PAYLOAD;
	case FPLINUX_UI_SETTING_STEP:
		return SETTING_STEP;
	default:
		break;
	}
	switch (setting->kind) {
	case FPLINUX_UI_SETTING_AUDIO_OUTPUT:
	case FPLINUX_UI_SETTING_AUDIO_OUTPUT_ACTIVE:
		return SETTING_ROUTE;
	case FPLINUX_UI_SETTING_AUDIO_OUTPUTS:
		return SETTING_ROUTES;
	case FPLINUX_UI_SETTING_SCREEN_OFF:
	case FPLINUX_UI_SETTING_AUTO_LOCK:
		return SETTING_SECONDS;
	case FPLINUX_UI_SETTING_LOCALE:
	case FPLINUX_UI_SETTING_INPUT_LOCALE:
		return SETTING_LOCALE;
	case FPLINUX_UI_SETTING_TIME:
		return SETTING_TIME;
	case FPLINUX_UI_SETTING_THEME:
		return SETTING_THEME;
	case FPLINUX_UI_SETTING_WIFI:
		return SETTING_ENABLED;
	case FPLINUX_UI_SETTING_MIDI_SOUNDFONT:
		return SETTING_SOUNDFONT;
	default:
		return SETTING_LEVEL;
	}
}

static bool setting_operation_valid(const struct fplinux_ui_setting *setting)
{
	if (setting->kind == FPLINUX_UI_SETTING_UNKNOWN)
		return setting->operation == FPLINUX_UI_SETTING_INVALID;
	if (setting->operation == FPLINUX_UI_SETTING_RESULT ||
	    setting->operation == FPLINUX_UI_SETTING_INVALID)
		return true;
	switch (setting->kind) {
	case FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW:
	case FPLINUX_UI_SETTING_VOLUME_PREVIEW:
	case FPLINUX_UI_SETTING_ALARM_VOLUME_PREVIEW:
		return setting->operation == FPLINUX_UI_SETTING_PREVIEW ||
		       setting->operation == FPLINUX_UI_SETTING_CANCEL;
	case FPLINUX_UI_SETTING_VOLUME_STEP:
	case FPLINUX_UI_SETTING_MEDIA_VOLUME_STEP:
		return setting->operation == FPLINUX_UI_SETTING_STEP;
	case FPLINUX_UI_SETTING_VOLUME_COMMIT:
	case FPLINUX_UI_SETTING_ALARM_VOLUME_COMMIT:
		return setting->operation == FPLINUX_UI_SETTING_COMMIT;
	case FPLINUX_UI_SETTING_AUDIO_OUTPUTS:
	case FPLINUX_UI_SETTING_AUDIO_OUTPUT_ACTIVE:
		return setting->operation == FPLINUX_UI_SETTING_GET;
	default:
		return setting->operation == FPLINUX_UI_SETTING_GET ||
		       setting->operation == FPLINUX_UI_SETTING_SET;
	}
}

static bool setting_time_shape(const struct fplinux_ui_setting_time *time)
{
	return time->year >= 0 && time->year <= 9999 && time->month >= 0 &&
	       time->month <= 99 && time->day >= 0 && time->day <= 99 &&
	       time->hour >= 0 && time->hour <= 99 && time->minute >= 0 &&
	       time->minute <= 99 && time->second >= 0 && time->second <= 99;
}

static bool setting_value_valid(const struct fplinux_ui_setting *setting)
{
	const union fplinux_ui_setting_value *value = &setting->value;
	unsigned int valid_routes =
		FPLINUX_UI_AUDIO_OUTPUT(FPLINUX_AUDIO_ROUTE_AUTO) |
		FPLINUX_UI_AUDIO_OUTPUT(FPLINUX_AUDIO_ROUTE_SPEAKER) |
		FPLINUX_UI_AUDIO_OUTPUT(FPLINUX_AUDIO_ROUTE_HEADPHONES);

	switch (setting_payload(setting)) {
	case SETTING_STEP:
		return value->step == -1 || value->step == 1;
	case SETTING_ROUTE:
		return (unsigned int)value->route < ARRAY_SIZE(route_names);
	case SETTING_ROUTES:
		return (value->routes &
			FPLINUX_UI_AUDIO_OUTPUT(FPLINUX_AUDIO_ROUTE_AUTO)) &&
		       !(value->routes & ~valid_routes);
	case SETTING_LOCALE:
		return value->locale[0] &&
		       memchr(value->locale, '\0', sizeof(value->locale));
	case SETTING_SOUNDFONT:
		return value->soundfont[0] &&
		       memchr(value->soundfont, '\0', sizeof(value->soundfont));
	case SETTING_TIME:
		return setting_time_shape(&value->time);
	case SETTING_THEME:
		return (unsigned int)value->theme < ARRAY_SIZE(theme_names);
	default:
		return true;
	}
}

static bool setting_valid(enum fplinux_ui_session_message_type type,
			  bool applied,
			  const struct fplinux_ui_setting *setting)
{
	return setting->kind > FPLINUX_UI_SETTING_NONE &&
	       (unsigned int)setting->kind < ARRAY_SIZE(setting_names) &&
	       setting_operation_valid(setting) &&
	       setting_value_valid(setting) &&
	       (!applied || setting->operation == FPLINUX_UI_SETTING_RESULT) &&
	       (type != FPLINUX_UI_SESSION_SETTING ||
		setting->operation != FPLINUX_UI_SETTING_RESULT || applied) &&
	       (type == FPLINUX_UI_SESSION_ERROR ||
		type == FPLINUX_UI_SESSION_SETTING);
}

static unsigned int name_index(const char *text, const char *const *names,
			       size_t count)
{
	for (size_t i = 0; i < count; ++i)
		if (names[i] && !strcmp(text, names[i]))
			return i;
	return count;
}

static bool setting_required(enum fplinux_ui_session_message_type type,
			     const struct fplinux_ui_session_message *message)
{
	return type == FPLINUX_UI_SESSION_SETTING ||
	       (type == FPLINUX_UI_SESSION_ERROR &&
		(message->applied || name_index(message->name, setting_names,
						ARRAY_SIZE(setting_names)) <
					     FPLINUX_UI_SETTING_UNKNOWN));
}

static bool
setting_fields_valid(const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_setting *setting = &message->setting;

	if (setting->kind == FPLINUX_UI_SETTING_NONE)
		return !setting_required(message->type, message);
	return setting_valid(message->type, message->applied, setting) &&
	       (setting->kind == FPLINUX_UI_SETTING_UNKNOWN ||
		!message->name[0]) &&
	       !message->value[0];
}

static void add_setting(struct json_object *object,
			const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_setting *setting = &message->setting;
	const union fplinux_ui_setting_value *value = &setting->value;
	enum setting_payload payload = setting_payload(setting);
	struct json_object *entry = json_object_new_object();
	struct json_object *field;

	add_string(entry, "kind", setting_names[setting->kind]);
	add_string(entry, "op", setting_operation_names[setting->operation]);
	if (setting->kind == FPLINUX_UI_SETTING_UNKNOWN)
		add_string(entry, "key", message->name);
	switch (payload) {
	case SETTING_NO_PAYLOAD:
		break;
	case SETTING_LEVEL:
		json_object_object_add(entry, "level",
				       json_object_new_int(value->level));
		break;
	case SETTING_STEP:
		json_object_object_add(entry, "step",
				       json_object_new_int(value->step));
		break;
	case SETTING_SECONDS:
		json_object_object_add(entry, "seconds",
				       json_object_new_int(value->seconds));
		break;
	case SETTING_ROUTE:
		add_string(entry, "route", route_names[value->route]);
		break;
	case SETTING_ROUTES:
		field = json_object_new_array();
		for (size_t i = 0; i < ARRAY_SIZE(route_names); ++i)
			if (value->routes & FPLINUX_UI_AUDIO_OUTPUT(i))
				json_object_array_add(
					field,
					json_object_new_string(route_names[i]));
		json_object_object_add(entry, "routes", field);
		break;
	case SETTING_LOCALE:
		add_string(entry, "locale", value->locale);
		break;
	case SETTING_SOUNDFONT:
		add_string(entry, "soundfont", value->soundfont);
		break;
	case SETTING_TIME:
		field = json_object_new_object();
		json_object_object_add(field, "year",
				       json_object_new_int(value->time.year));
		json_object_object_add(field, "month",
				       json_object_new_int(value->time.month));
		json_object_object_add(field, "day",
				       json_object_new_int(value->time.day));
		json_object_object_add(field, "hour",
				       json_object_new_int(value->time.hour));
		json_object_object_add(field, "minute",
				       json_object_new_int(value->time.minute));
		json_object_object_add(field, "second",
				       json_object_new_int(value->time.second));
		json_object_object_add(entry, "time", field);
		break;
	case SETTING_THEME:
		add_string(entry, "theme", theme_names[value->theme]);
		break;
	case SETTING_ENABLED:
		json_object_object_add(entry, "enabled",
				       json_object_new_boolean(value->enabled));
		break;
	}
	json_object_object_add(object, "setting", entry);
}

static bool required_int(struct json_object *object, const char *key,
			 int *value)
{
	struct json_object *field;

	return json_object_object_get_ex(object, key, &field) &&
	       get_int(object, key, value);
}

static bool get_setting_routes(struct json_object *entry, unsigned int *routes)
{
	struct json_object *field;

	if (!json_object_object_get_ex(entry, "routes", &field) ||
	    !json_object_is_type(field, json_type_array) ||
	    json_object_array_length(field) > ARRAY_SIZE(route_names))
		return false;
	for (size_t i = 0; i < json_object_array_length(field); ++i) {
		struct json_object *route = json_object_array_get_idx(field, i);
		const char *name;
		unsigned int choice;

		if (!json_object_is_type(route, json_type_string))
			return false;
		name = json_object_get_string(route);
		choice = name_index(name, route_names, ARRAY_SIZE(route_names));
		if (strlen(name) != (size_t)json_object_get_string_len(route) ||
		    choice >= ARRAY_SIZE(route_names) ||
		    (*routes & FPLINUX_UI_AUDIO_OUTPUT(choice)))
			return false;
		*routes |= FPLINUX_UI_AUDIO_OUTPUT(choice);
	}
	return true;
}

static bool get_setting_payload(struct json_object *entry,
				struct fplinux_ui_setting *setting)
{
	union fplinux_ui_setting_value *value = &setting->value;
	struct json_object *field;
	unsigned int choice;

	switch (setting_payload(setting)) {
	case SETTING_NO_PAYLOAD:
		return true;
	case SETTING_LEVEL:
		return required_int(entry, "level", &value->level);
	case SETTING_STEP:
		return required_int(entry, "step", &value->step);
	case SETTING_SECONDS:
		return required_int(entry, "seconds", &value->seconds);
	case SETTING_ROUTE:
		if (!get_choice(entry, "route", route_names,
				ARRAY_SIZE(route_names),
				ARRAY_SIZE(route_names), &choice))
			return false;
		value->route = choice;
		return true;
	case SETTING_ROUTES:
		return get_setting_routes(entry, &value->routes);
	case SETTING_LOCALE:
		return get_string(entry, "locale", value->locale,
				  sizeof(value->locale));
	case SETTING_SOUNDFONT:
		return get_string(entry, "soundfont", value->soundfont,
				  sizeof(value->soundfont));
	case SETTING_TIME:
		return json_object_object_get_ex(entry, "time", &field) &&
		       json_object_is_type(field, json_type_object) &&
		       json_object_object_length(field) == 6 &&
		       required_int(field, "year", &value->time.year) &&
		       required_int(field, "month", &value->time.month) &&
		       required_int(field, "day", &value->time.day) &&
		       required_int(field, "hour", &value->time.hour) &&
		       required_int(field, "minute", &value->time.minute) &&
		       required_int(field, "second", &value->time.second);
	case SETTING_THEME:
		if (!get_choice(entry, "theme", theme_names,
				ARRAY_SIZE(theme_names),
				ARRAY_SIZE(theme_names), &choice))
			return false;
		value->theme = choice;
		return true;
	case SETTING_ENABLED:
		return json_object_object_get_ex(entry, "enabled", &field) &&
		       get_bool(entry, "enabled", &value->enabled);
	}
	return false;
}

static bool get_setting(struct json_object *object,
			enum fplinux_ui_session_message_type type,
			struct fplinux_ui_session_message *message)
{
	struct fplinux_ui_setting *setting = &message->setting;
	struct json_object *entry, *field;
	unsigned int kind, operation;
	bool has_key;

	if (!json_object_object_get_ex(object, "setting", &entry))
		return !setting_required(type, message);
	if (!json_object_is_type(entry, json_type_object) ||
	    json_object_object_get_ex(object, "name", &field) ||
	    json_object_object_get_ex(object, "value", &field) ||
	    !get_choice(entry, "kind", setting_names, ARRAY_SIZE(setting_names),
			FPLINUX_UI_SETTING_NONE, &kind) ||
	    !get_choice(entry, "op", setting_operation_names,
			ARRAY_SIZE(setting_operation_names),
			ARRAY_SIZE(setting_operation_names), &operation) ||
	    operation >= ARRAY_SIZE(setting_operation_names))
		return false;
	setting->kind = kind;
	setting->operation = operation;
	has_key = kind == FPLINUX_UI_SETTING_UNKNOWN &&
		  json_object_object_get_ex(entry, "key", &field);
	if (json_object_object_length(entry) !=
		    2 + (has_key ||
			 setting_payload(setting) != SETTING_NO_PAYLOAD) ||
	    !get_setting_payload(entry, setting) ||
	    !setting_valid(type, message->applied, setting))
		return false;
	return !has_key ||
	       get_string(entry, "key", message->name, sizeof(message->name));
}

static bool get_launcher_view(struct json_object *object,
			      enum fplinux_ui_session_message_type type,
			      struct fplinux_ui_session_message *message)
{
	struct json_object *field;
	unsigned int view, catalogue;

	if (type != FPLINUX_UI_SESSION_ACTIVE &&
	    type != FPLINUX_UI_SESSION_SELECT)
		return !json_object_object_get_ex(object, "launcher_view",
						  &field) &&
		       !json_object_object_get_ex(object, "launcher_catalogue",
						  &field);
	if (json_object_object_get_ex(object, "name", &field) ||
	    json_object_object_get_ex(object, "value", &field) ||
	    !get_choice(object, "launcher_view", launcher_view_names,
			ARRAY_SIZE(launcher_view_names),
			FPLINUX_UI_LAUNCHER_VIEW_NONE, &view) ||
	    !get_choice(object, "launcher_catalogue", launcher_catalogue_names,
			ARRAY_SIZE(launcher_catalogue_names),
			FPLINUX_UI_LAUNCHER_CATALOGUE_NONE, &catalogue) ||
	    (type == FPLINUX_UI_SESSION_ACTIVE &&
	     view > FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE) ||
	    (view > FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE &&
	     (catalogue != FPLINUX_UI_LAUNCHER_CATALOGUE_NONE ||
	      message->selected_id[0])))
		return false;
	message->launcher_view = view;
	message->launcher_catalogue = catalogue;
	return true;
}

static bool get_close_response(struct json_object *object,
			       struct fplinux_ui_session_message *message)
{
	unsigned int response;

	if (!get_choice(object, "value", close_response_names,
			ARRAY_SIZE(close_response_names),
			FPLINUX_UI_SESSION_CLOSE_NONE, &response) ||
	    response == FPLINUX_UI_SESSION_CLOSE_NONE)
		return false;
	message->close_response = response;
	return true;
}

static bool volume_text_valid(const char *text, size_t capacity)
{
	size_t length = strnlen(text, capacity);

	if (length == capacity)
		return false;
	for (size_t i = 0; i < length; ++i)
		if ((unsigned char)text[i] < 0x20 || text[i] == 0x7f)
			return false;
	return true;
}

static bool volume_valid(const struct fplinux_volume *volume)
{
	if (!volume->id ||
	    (unsigned int)volume->state > FPLINUX_VOLUME_PROTECTED ||
	    (unsigned int)volume->protection > FPLINUX_VOLUME_OTHER_MOUNT ||
	    !volume->label[0] ||
	    !volume_text_valid(volume->label, sizeof(volume->label)) ||
	    !volume_text_valid(volume->path, sizeof(volume->path)))
		return false;
	if ((volume->state == FPLINUX_VOLUME_PROTECTED) !=
	    (volume->protection != FPLINUX_VOLUME_UNPROTECTED))
		return false;
	return volume->state == FPLINUX_VOLUME_MOUNTED ?
		       volume->path[0] == '/' && volume->mount_id :
		       !volume->path[0] && !volume->mount_id;
}

static bool
volume_fields_valid(const struct fplinux_ui_session_message *message)
{
	const struct fplinux_volume_result *result = &message->volume_result;

	if (message->type == FPLINUX_UI_SESSION_STORAGE_REQUEST)
		return message->serial &&
		       (unsigned int)message->volume_operation <
			       ARRAY_SIZE(volume_operation_names) &&
		       (message->volume_operation == FPLINUX_VOLUME_REFRESH ?
				!message->volume_id :
				message->volume_id != 0) &&
		       !result->request_id && !result->error &&
		       !result->snapshot.count &&
		       !result->snapshot.home_persistent;
	if (message->volume_id || message->volume_operation)
		return false;
	if (message->type != FPLINUX_UI_SESSION_STORAGE_RESULT)
		return !result->request_id && !result->error &&
		       !result->snapshot.count &&
		       !result->snapshot.home_persistent;
	if (!message->serial || result->request_id != message->serial ||
	    result->error < 0 || result->snapshot.count > FPLINUX_VOLUME_MAX)
		return false;
	for (size_t i = 0; i < result->snapshot.count; ++i) {
		if (!volume_valid(&result->snapshot.volumes[i]))
			return false;
		for (size_t j = 0; j < i; ++j)
			if (result->snapshot.volumes[i].id ==
			    result->snapshot.volumes[j].id)
				return false;
	}
	return true;
}

static bool get_volume_id(struct json_object *object, const char *name,
			  uint32_t *id)
{
	struct json_object *field;
	int64_t number;

	if (!json_object_object_get_ex(object, name, &field) ||
	    !json_object_is_type(field, json_type_int))
		return false;
	number = json_object_get_int64(field);
	if (number < 0 || number > UINT32_MAX)
		return false;
	*id = (uint32_t)number;
	return true;
}

static bool get_volume(struct json_object *object,
		       struct fplinux_volume *volume)
{
	static const char *const fields[] = {
		"id",	      "mount_id",  "state",
		"protection", "read_only", "shared_system_files",
		"label",      "path",
	};
	struct json_object *field;
	int state = 0, protection = 0;

	if (!json_object_is_type(object, json_type_object) ||
	    (size_t)json_object_object_length(object) != ARRAY_SIZE(fields))
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(fields); ++i)
		if (!json_object_object_get_ex(object, fields[i], &field))
			return false;
	if (!get_volume_id(object, "id", &volume->id) ||
	    !get_volume_id(object, "mount_id", &volume->mount_id) ||
	    !get_int(object, "state", &state) ||
	    !get_int(object, "protection", &protection) ||
	    !get_bool(object, "read_only", &volume->read_only) ||
	    !get_bool(object, "shared_system_files",
		      &volume->shared_system_files) ||
	    !get_string(object, "label", volume->label,
			sizeof(volume->label)) ||
	    !get_string(object, "path", volume->path, sizeof(volume->path)))
		return false;
	volume->state = state;
	volume->protection = protection;
	return volume_valid(volume);
}

static bool get_volume_fields(struct json_object *object, unsigned int type,
			      struct fplinux_ui_session_message *message)
{
	static const char *const fields[] = {
		"volume_operation", "volume_id", "volume_error",
		"home_persistent",  "volumes",
	};
	struct json_object *field, *array;
	struct fplinux_volume_result *result = &message->volume_result;

	for (size_t i = 0; i < ARRAY_SIZE(fields); ++i) {
		bool present =
			json_object_object_get_ex(object, fields[i], &field);
		bool required =
			type == FPLINUX_UI_SESSION_STORAGE_REQUEST ? i < 2 :
			type == FPLINUX_UI_SESSION_STORAGE_RESULT  ? i >= 2 :
								     false;

		if (present != required)
			return false;
	}
	if (type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
		unsigned int operation = ARRAY_SIZE(volume_operation_names);

		if (!get_choice(
			    object, "volume_operation", volume_operation_names,
			    ARRAY_SIZE(volume_operation_names),
			    ARRAY_SIZE(volume_operation_names), &operation) ||
		    operation >= ARRAY_SIZE(volume_operation_names) ||
		    !get_volume_id(object, "volume_id", &message->volume_id))
			return false;
		message->volume_operation = operation;
	}
	if (type != FPLINUX_UI_SESSION_STORAGE_RESULT)
		return true;
	if (!get_int(object, "volume_error", &result->error) ||
	    !get_bool(object, "home_persistent",
		      &result->snapshot.home_persistent) ||
	    !json_object_object_get_ex(object, "volumes", &array) ||
	    !json_object_is_type(array, json_type_array))
		return false;
	result->snapshot.count = json_object_array_length(array);
	if (result->snapshot.count > FPLINUX_VOLUME_MAX)
		return false;
	for (size_t i = 0; i < result->snapshot.count; ++i)
		if (!get_volume(json_object_array_get_idx(array, i),
				&result->snapshot.volumes[i]))
			return false;
	return true;
}

static void add_volume_result(struct json_object *object,
			      const struct fplinux_volume_result *result)
{
	struct json_object *array = json_object_new_array();

	json_object_object_add(object, "volume_error",
			       json_object_new_int(result->error));
	json_object_object_add(
		object, "home_persistent",
		json_object_new_boolean(result->snapshot.home_persistent));
	for (size_t i = 0; i < result->snapshot.count; ++i) {
		const struct fplinux_volume *volume =
			&result->snapshot.volumes[i];
		struct json_object *entry = json_object_new_object();

		json_object_object_add(entry, "id",
				       json_object_new_int64(volume->id));
		json_object_object_add(entry, "mount_id",
				       json_object_new_int64(volume->mount_id));
		json_object_object_add(entry, "state",
				       json_object_new_int(volume->state));
		json_object_object_add(entry, "protection",
				       json_object_new_int(volume->protection));
		json_object_object_add(
			entry, "read_only",
			json_object_new_boolean(volume->read_only));
		json_object_object_add(
			entry, "shared_system_files",
			json_object_new_boolean(volume->shared_system_files));
		json_object_object_add(entry, "label",
				       json_object_new_string(volume->label));
		json_object_object_add(entry, "path",
				       json_object_new_string(volume->path));
		json_object_array_add(array, entry);
	}
	json_object_object_add(object, "volumes", array);
}

static bool bluetooth_fields_valid(const struct fplinux_ui_session_message *m)
{
	const struct fplinux_ui_bluetooth_prompt *prompt = &m->bluetooth_prompt;

	if (m->type == FPLINUX_UI_SESSION_BLUETOOTH_ACTION) {
		static const char *const actions[] = {
			"accept", "decline", "hide", "close", "open", "folder",
		};

		if (!m->serial)
			return false;
		for (size_t i = 0; i < ARRAY_SIZE(actions); ++i)
			if (!strcmp(m->value, actions[i]))
				return true;
		return false;
	}
	if (prompt->phase == FPLINUX_UI_BLUETOOTH_IDLE)
		return prompt->id == 0;
	return m->type == FPLINUX_UI_SESSION_ACTIVE && prompt->id &&
	       (unsigned int)prompt->phase <= FPLINUX_UI_BLUETOOTH_FAILED &&
	       prompt->filename[0] &&
	       strnlen(prompt->filename, sizeof(prompt->filename)) <
		       sizeof(prompt->filename) &&
	       strnlen(prompt->peer, sizeof(prompt->peer)) <
		       sizeof(prompt->peer) &&
	       (!prompt->can_open ||
		prompt->phase == FPLINUX_UI_BLUETOOTH_COMPLETE);
}

static bool get_bluetooth_bytes(struct json_object *object, const char *field,
				uint64_t *value)
{
	struct json_object *number;

	if (!json_object_object_get_ex(object, field, &number) ||
	    !json_object_is_type(number, json_type_int) ||
	    json_object_get_int64(number) < 0)
		return false;
	*value = json_object_get_uint64(number);
	return true;
}

static bool get_bluetooth_prompt(struct json_object *object,
				 struct fplinux_ui_session_message *m)
{
	struct fplinux_ui_bluetooth_prompt *prompt = &m->bluetooth_prompt;
	struct json_object *state;
	int phase = 0;

	if (!json_object_object_get_ex(object, "bluetooth_prompt", &state))
		return true;
	if (m->type != FPLINUX_UI_SESSION_ACTIVE ||
	    !json_object_is_type(state, json_type_object) ||
	    !get_token(state, &prompt->id) ||
	    !get_int(state, "phase", &phase) || phase <= 0 ||
	    phase > FPLINUX_UI_BLUETOOTH_FAILED ||
	    !get_string(state, "peer", prompt->peer, sizeof(prompt->peer)) ||
	    !get_string(state, "filename", prompt->filename,
			sizeof(prompt->filename)) ||
	    !get_bluetooth_bytes(state, "transferred",
				 &prompt->transferred_bytes) ||
	    !get_bluetooth_bytes(state, "total", &prompt->total_bytes) ||
	    !get_bool(state, "total_known", &prompt->total_known) ||
	    !get_bool(state, "can_open", &prompt->can_open))
		return false;
	prompt->phase = phase;
	return true;
}

static void
add_bluetooth_prompt(struct json_object *object,
		     const struct fplinux_ui_bluetooth_prompt *prompt)
{
	struct json_object *state;

	if (prompt->phase == FPLINUX_UI_BLUETOOTH_IDLE)
		return;
	state = json_object_new_object();
	json_object_object_add(state, "token",
			       json_object_new_int64(prompt->id));
	json_object_object_add(state, "phase",
			       json_object_new_int(prompt->phase));
	add_string(state, "peer", prompt->peer);
	add_string(state, "filename", prompt->filename);
	json_object_object_add(
		state, "transferred",
		json_object_new_uint64(prompt->transferred_bytes));
	json_object_object_add(state, "total",
			       json_object_new_uint64(prompt->total_bytes));
	json_object_object_add(state, "total_known",
			       json_object_new_boolean(prompt->total_known));
	json_object_object_add(state, "can_open",
			       json_object_new_boolean(prompt->can_open));
	json_object_object_add(object, "bluetooth_prompt", state);
}

static bool is_document_type(unsigned int type)
{
	return (type >= FPLINUX_UI_SESSION_OPEN_FILE &&
		type <= FPLINUX_UI_SESSION_DOCUMENT_RESULT) ||
	       type == FPLINUX_UI_SESSION_BLUETOOTH_SEND;
}

static bool document_has_path(unsigned int type)
{
	return type == FPLINUX_UI_SESSION_OPEN_FILE ||
	       type == FPLINUX_UI_SESSION_DOCUMENT ||
	       type == FPLINUX_UI_SESSION_BLUETOOTH_SEND;
}

static bool document_fields_valid(const struct fplinux_ui_session_message *m)
{
	if (!is_document_type(m->type))
		return !m->path[0] && !m->document_error &&
		       !m->document_status && !m->document_choice_count;
	if (!m->serial ||
	    m->document_choice_count > FPLINUX_UI_SESSION_DOCUMENT_CHOICE_MAX)
		return false;
	if (m->type != FPLINUX_UI_SESSION_DOCUMENT_RESULT &&
	    (m->document_status || m->document_error))
		return false;
	if (m->type != FPLINUX_UI_SESSION_DOCUMENT_CHOICES &&
	    m->document_choice_count)
		return false;
	if (!document_has_path(m->type) && m->path[0])
		return false;
	if (document_has_path(m->type))
		return m->path[0] == '/' &&
		       strnlen(m->path, sizeof(m->path)) < sizeof(m->path);
	if (m->type == FPLINUX_UI_SESSION_DOCUMENT_RESULT)
		return (unsigned int)m->document_status <
			       ARRAY_SIZE(document_status_names) &&
		       (m->document_status == FPLINUX_UI_DOCUMENT_ERROR ?
				m->document_error > 0 :
				!m->document_error);
	if (m->type == FPLINUX_UI_SESSION_DOCUMENT_CHOICE)
		return !m->app_id[0] || task_id_valid(m->app_id);
	if (!m->document_choice_count)
		return false;
	for (unsigned int i = 0; i < m->document_choice_count; ++i)
		if (!task_id_valid(m->document_choices[i].app_id) ||
		    !m->document_choices[i].title[0])
			return false;
	return true;
}

static bool get_document_path(struct json_object *object, char *path,
			      size_t capacity)
{
	struct json_object *field;
	const char *value;
	size_t bytes;

	if (!json_object_object_get_ex(object, "path", &field))
		return true;
	if (!json_object_is_type(field, json_type_string))
		return false;
	value = json_object_get_string(field);
	bytes = json_object_get_string_len(field);
	if (bytes >= capacity || strlen(value) != bytes)
		return false;
	/* File names remain data; unlike labels they may contain control bytes. */
	memcpy(path, value, bytes + 1);
	return true;
}

static bool get_document_fields(struct json_object *object, unsigned int type,
				struct fplinux_ui_session_message *m)
{
	struct json_object *array;
	struct json_object *field;
	unsigned int status = 0;

	if (!is_document_type(type))
		return !json_object_object_get_ex(object, "path", &field) &&
		       !json_object_object_get_ex(object, "document_status",
						  &field) &&
		       !json_object_object_get_ex(object, "document_error",
						  &field) &&
		       !json_object_object_get_ex(object, "document_choices",
						  &field);
	if ((json_object_object_get_ex(object, "path", &field) &&
	     !document_has_path(type)) ||
	    ((json_object_object_get_ex(object, "document_status", &field) ||
	      json_object_object_get_ex(object, "document_error", &field)) &&
	     type != FPLINUX_UI_SESSION_DOCUMENT_RESULT) ||
	    (type == FPLINUX_UI_SESSION_DOCUMENT_RESULT &&
	     !json_object_object_get_ex(object, "document_status", &field)))
		return false;
	if (!get_document_path(object, m->path, sizeof(m->path)) ||
	    !get_int(object, "document_error", &m->document_error) ||
	    !get_choice(object, "document_status", document_status_names,
			ARRAY_SIZE(document_status_names), 0, &status))
		return false;
	m->document_status = status;
	if (json_object_object_get_ex(object, "document_choices", &array)) {
		size_t count;

		if (type != FPLINUX_UI_SESSION_DOCUMENT_CHOICES ||
		    !json_object_is_type(array, json_type_array))
			return false;
		count = json_object_array_length(array);
		if (count > FPLINUX_UI_SESSION_DOCUMENT_CHOICE_MAX)
			return false;
		m->document_choice_count = count;
		for (size_t i = 0; i < count; ++i) {
			struct json_object *entry =
				json_object_array_get_idx(array, i);
			struct fplinux_ui_session_document_choice *choice =
				&m->document_choices[i];

			if (!json_object_is_type(entry, json_type_object) ||
			    json_object_object_length(entry) != 3 ||
			    !get_string(entry, "app_id", choice->app_id,
					sizeof(choice->app_id)) ||
			    !get_string(entry, "title", choice->title,
					sizeof(choice->title)) ||
			    !get_string(entry, "icon", choice->icon,
					sizeof(choice->icon)))
				return false;
		}
	}
	m->type = type;
	return true;
}

static bool
storage_result_valid(const struct fplinux_ui_session_message *message)
{
	if ((unsigned int)message->storage_state >= ARRAY_SIZE(storage_names) ||
	    message->storage_error < 0)
		return false;
	if (message->type != FPLINUX_UI_SESSION_SETTING &&
	    message->type != FPLINUX_UI_SESSION_ERROR)
		return !message->applied && !message->storage_state &&
		       !message->storage_error;
	if (message->storage_state == FPLINUX_STORAGE_DURABLE)
		return message->applied && !message->storage_error;
	if (message->storage_state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED)
		return message->applied && message->storage_error;
	return true;
}

static bool get_storage_fields(struct json_object *object, unsigned int type,
			       struct fplinux_ui_session_message *message)
{
	struct json_object *field;
	unsigned int state;
	bool has_storage =
		json_object_object_get_ex(object, "applied", &field) ||
		json_object_object_get_ex(object, "storage_state", &field) ||
		json_object_object_get_ex(object, "storage_error", &field);

	if (has_storage && type != FPLINUX_UI_SESSION_SETTING &&
	    type != FPLINUX_UI_SESSION_ERROR)
		return false;
	if (!get_bool(object, "applied", &message->applied) ||
	    !get_int(object, "storage_error", &message->storage_error) ||
	    !get_choice(object, "storage_state", storage_names,
			ARRAY_SIZE(storage_names), FPLINUX_STORAGE_UNPUBLISHED,
			&state))
		return false;
	message->storage_state = state;
	message->type = type;
	return storage_result_valid(message);
}

bool fplinux_ui_session_vibrate_valid(
	const struct fplinux_ui_session_message *message)
{
	const unsigned int *pattern = message->pattern;
	unsigned int count = message->pattern_count;
	unsigned int last_pulse;
	unsigned int pause;

	if (message->type != FPLINUX_UI_SESSION_VIBRATE ||
	    count > FPLINUX_UI_SESSION_PATTERN)
		return false;
	if (!count)
		return !message->repeat;
	if (count < 2 || pattern[0] > VIBRATION_PAUSE_MAX_MS)
		return false;
	for (unsigned int i = 1; i < count; ++i) {
		bool pulse = i % 2;
		unsigned int min = pulse	 ? VIBRATION_PULSE_MIN_MS :
				   i + 1 < count ? VIBRATION_PAUSE_MIN_MS :
						   0;
		unsigned int max = pulse ? VIBRATION_PULSE_MAX_MS :
					   VIBRATION_PAUSE_MAX_MS;

		if (pattern[i] < min || pattern[i] > max)
			return false;
	}
	if (!message->repeat)
		return true;
	/* The final pause and initial wait also separate repeated pulses. */
	last_pulse = count % 2 ? count - 2 : count - 1;
	pause = pattern[0] + (count % 2 ? pattern[count - 1] : 0);
	if (pause)
		return pause >= VIBRATION_PAUSE_MIN_MS;
	/* With no pause, distinct last and first pulses join into one pulse. */
	return last_pulse > 1 &&
	       pattern[last_pulse] + pattern[1] <= VIBRATION_PULSE_MAX_MS;
}

bool fplinux_ui_session_light_effect_valid(
	const struct fplinux_ui_light_effect *effect)
{
	unsigned int mask = FPLINUX_UI_LIGHT_SCREEN | FPLINUX_UI_LIGHT_KEYPAD;

	return effect && !(effect->resources & ~mask) &&
	       (effect->resources & FPLINUX_UI_LIGHT_SCREEN ?
			effect->display_level >= 1 &&
				effect->display_level <= 10 :
			!effect->display_level) &&
	       ((effect->resources & FPLINUX_UI_LIGHT_KEYPAD) ||
		!effect->keypad_on);
}

static bool light_fields_valid(const struct fplinux_ui_session_message *message)
{
	const struct fplinux_ui_light_effect *effect = &message->light_effect;

	if (message->light_effects > 3 ||
	    (message->light_effects &&
	     message->type != FPLINUX_UI_SESSION_CONFIG))
		return false;
	if (message->type == FPLINUX_UI_SESSION_LIGHT_EFFECT)
		return message->serial &&
		       fplinux_ui_session_light_effect_valid(effect);
	return !effect->resources && !effect->display_level &&
	       !effect->keypad_on;
}

static bool get_light_fields(struct json_object *object, unsigned int type,
			     struct fplinux_ui_session_message *message)
{
	struct json_object *state, *field;
	int resources = 0, level = 0, supported = 0;
	bool has_effect =
		json_object_object_get_ex(object, "light_effect", &state);

	if (json_object_object_get_ex(object, "light_effects", &field) &&
	    (type != FPLINUX_UI_SESSION_CONFIG ||
	     !get_int(object, "light_effects", &supported) || supported < 0 ||
	     supported > 3))
		return false;
	message->light_effects = supported;
	if (type != FPLINUX_UI_SESSION_LIGHT_EFFECT)
		return !has_effect;
	if (!has_effect || !json_object_is_type(state, json_type_object) ||
	    json_object_object_length(state) != 3 ||
	    !json_object_object_get_ex(state, "resources", &field) ||
	    !get_int(state, "resources", &resources) || resources < 0 ||
	    !json_object_object_get_ex(state, "display_level", &field) ||
	    !get_int(state, "display_level", &level) || level < 0 ||
	    !json_object_object_get_ex(state, "keypad_on", &field) ||
	    !get_bool(state, "keypad_on", &message->light_effect.keypad_on))
		return false;
	message->light_effect.resources = resources;
	message->light_effect.display_level = level;
	return fplinux_ui_session_light_effect_valid(&message->light_effect);
}

static bool get_media_fields(struct json_object *object, unsigned int type,
			     struct fplinux_ui_session_message *message)
{
	struct json_object *field;
	struct json_object *pattern;
	size_t count;

	if ((json_object_object_get_ex(object, "audio", &field) ||
	     json_object_object_get_ex(object, "background_audio", &field) ||
	     json_object_object_get_ex(object, "vibration", &field)) &&
	    type != FPLINUX_UI_SESSION_CONFIG)
		return false;
	if (!get_bool(object, "audio", &message->audio) ||
	    !get_bool(object, "background_audio", &message->background_audio) ||
	    !get_bool(object, "vibration", &message->vibration))
		return false;
	if (message->background_audio && !message->audio)
		return false;
	if (type != FPLINUX_UI_SESSION_VIBRATE)
		return !json_object_object_get_ex(object, "pattern", &field) &&
		       !json_object_object_get_ex(object, "repeat", &field);
	if (!json_object_object_get_ex(object, "pattern", &pattern) ||
	    !json_object_is_type(pattern, json_type_array) ||
	    !get_bool(object, "repeat", &message->repeat))
		return false;
	count = json_object_array_length(pattern);
	if (count > FPLINUX_UI_SESSION_PATTERN)
		return false;
	message->pattern_count = count;
	for (size_t i = 0; i < count; ++i) {
		int64_t duration;

		field = json_object_array_get_idx(pattern, i);
		if (!json_object_is_type(field, json_type_int))
			return false;
		duration = json_object_get_int64(field);
		if (duration < 0 || duration > VIBRATION_PAUSE_MAX_MS)
			return false;
		message->pattern[i] = duration;
	}
	message->type = type;
	return fplinux_ui_session_vibrate_valid(message);
}

static bool
frame_dimensions_valid(const struct fplinux_ui_session_message *message)
{
	return message->width > 0 &&
	       message->width <= FPLINUX_UI_SESSION_FRAME_MAX_WIDTH &&
	       message->height > 0 &&
	       message->height <= FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT &&
	       (size_t)message->width * (size_t)message->height <=
		       FPLINUX_UI_SESSION_FRAME_MAX_PIXELS;
}

static bool
frame_message_valid(const struct fplinux_ui_session_message *message)
{
	return message->type == FPLINUX_UI_SESSION_LAUNCH;
}

static bool input_locale_valid(const char *locale)
{
	size_t bytes = strnlen(locale, FPLINUX_UI_SESSION_LOCALE_BYTES);

	/* Empty retains the client's language fallback until state arrives. */
	if (!bytes)
		return true;
	if (bytes != 2 && bytes != 3)
		return false;
	for (size_t i = 0; i < bytes; ++i)
		if (locale[i] < 'a' || locale[i] > 'z')
			return false;
	return true;
}

bool fplinux_ui_session_send(int fd,
			     const struct fplinux_ui_session_message *message)
{
	unsigned int launcher_view = message->launcher_view;
	unsigned int launcher_catalogue = message->launcher_catalogue;
	unsigned int close_response = message->close_response;
	bool launcher = message->type == FPLINUX_UI_SESSION_SELECT ||
			message->type == FPLINUX_UI_SESSION_ACTIVE;
	struct json_object *object;
	const char *packet;
	size_t length;
	ssize_t sent;
	int error;

	if ((unsigned int)message->type >= ARRAY_SIZE(message_names) ||
	    (unsigned int)message->mode >= ARRAY_SIZE(mode_names) ||
	    (unsigned int)message->display >= ARRAY_SIZE(display_names) ||
	    (unsigned int)message->theme >= ARRAY_SIZE(theme_names) ||
	    !input_locale_valid(message->input_locale) ||
	    !clock_data_valid(message->clock_data) ||
	    !clock_presentation_valid(message) ||
	    !storage_result_valid(message) || !document_fields_valid(message) ||
	    !volume_fields_valid(message) || !system_levels_valid(message) ||
	    !light_fields_valid(message) || !wifi_fields_valid(message) ||
	    !bluetooth_fields_valid(message) ||
	    !setting_fields_valid(message) ||
	    (launcher &&
	     (message->name[0] || message->value[0] ||
	      launcher_view >= ARRAY_SIZE(launcher_view_names) ||
	      launcher_catalogue >= ARRAY_SIZE(launcher_catalogue_names) ||
	      (message->type == FPLINUX_UI_SESSION_ACTIVE &&
	       launcher_view > FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE) ||
	      (launcher_view > FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE &&
	       (launcher_catalogue || message->selected_id[0])))) ||
	    (message->tasks_view &&
	     message->type != FPLINUX_UI_SESSION_ACTIVE) ||
	    (message->background_playing &&
	     message->type != FPLINUX_UI_SESSION_ACTIVE) ||
	    ((message->audio || message->background_audio ||
	      message->vibration) &&
	     message->type != FPLINUX_UI_SESSION_CONFIG) ||
	    (message->background_audio && !message->audio) ||
	    ((message->pattern_count || message->repeat) &&
	     message->type != FPLINUX_UI_SESSION_VIBRATE) ||
	    (message->type == FPLINUX_UI_SESSION_VIBRATE &&
	     !fplinux_ui_session_vibrate_valid(message)) ||
	    (message->input_locale[0] &&
	     message->type != FPLINUX_UI_SESSION_CONFIG &&
	     message->type != FPLINUX_UI_SESSION_ACTIVE) ||
	    (message->clock_data[0] &&
	     message->type != FPLINUX_UI_SESSION_CLOCK &&
	     message->type != FPLINUX_UI_SESSION_ACTIVE) ||
	    (message->type == FPLINUX_UI_SESSION_LIFECYCLE &&
	     (unsigned int)message->lifecycle >= ARRAY_SIZE(lifecycle_names)) ||
	    (message->type == FPLINUX_UI_SESSION_ACTIVATION &&
	     (unsigned int)message->activation >=
		     ARRAY_SIZE(activation_names)) ||
	    (message->type != FPLINUX_UI_SESSION_ACTIVATION &&
	     message->activation != FPLINUX_UI_SESSION_ACTIVATION_OPEN) ||
	    (message->type == FPLINUX_UI_SESSION_TASKS &&
	     !tasks_valid(message)) ||
	    ((message->type == FPLINUX_UI_SESSION_ACTIVATE ||
	      message->type == FPLINUX_UI_SESSION_CLOSE_TASK) &&
	     (!task_id_valid(message->app_id) || !message->token)) ||
	    (message->type == FPLINUX_UI_SESSION_ACTIVE && message->token &&
	     !task_id_valid(message->active_id)) ||
	    (message->has_frame &&
	     (!frame_message_valid(message) || message->frame_fd < 0 ||
	      !frame_dimensions_valid(message))) ||
	    ((message->type == FPLINUX_UI_SESSION_ACTIVE_ACK ||
	      message->type == FPLINUX_UI_SESSION_ACTIVATION ||
	      message->type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
	      message->type == FPLINUX_UI_SESSION_REDRAW_ACK ||
	      message->type == FPLINUX_UI_SESSION_CLOSE_ACK ||
	      message->type == FPLINUX_UI_SESSION_PREPARE_CLOSE ||
	      message->type == FPLINUX_UI_SESSION_CLOSE_REPLY) &&
	     !message->serial) ||
	    (message->type == FPLINUX_UI_SESSION_CLOSE_REPLY &&
	     (message->value[0] || !close_response ||
	      close_response >= ARRAY_SIZE(close_response_names)))) {
		errno = EINVAL;
		return false;
	}
	object = json_object_new_object();
	if (!object)
		return false;
	if (!add_clock(object, message->clock_data)) {
		json_object_put(object);
		return false;
	}
	if (message->type == FPLINUX_UI_SESSION_ACTIVE)
		add_clock_presentation(object, &message->clock_presentation);
	add_string(object, "type", message_names[message->type]);
	add_wifi_fields(object, message);
	add_bluetooth_prompt(object, &message->bluetooth_prompt);
	if (message->type == FPLINUX_UI_SESSION_SYSTEM_LEVELS)
		add_system_levels(object, &message->system_levels);
	if (message->type == FPLINUX_UI_SESSION_STORAGE_REQUEST ||
	    message->type == FPLINUX_UI_SESSION_STORAGE_RESULT) {
		json_object_object_add(object, "serial",
				       json_object_new_int64(message->serial));
		if (message->type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
			add_string(object, "volume_operation",
				   volume_operation_names
					   [message->volume_operation]);
			json_object_object_add(
				object, "volume_id",
				json_object_new_int64(message->volume_id));
		} else {
			add_volume_result(object, &message->volume_result);
		}
	}
	if (is_document_type(message->type)) {
		add_string(object, "path", message->path);
		json_object_object_add(object, "serial",
				       json_object_new_int64(message->serial));
		if (message->type == FPLINUX_UI_SESSION_DOCUMENT_RESULT) {
			add_string(
				object, "document_status",
				document_status_names[message->document_status]);
			json_object_object_add(
				object, "document_error",
				json_object_new_int(message->document_error));
		}
		if (message->type == FPLINUX_UI_SESSION_DOCUMENT_CHOICES) {
			struct json_object *choices = json_object_new_array();

			for (unsigned int i = 0;
			     i < message->document_choice_count; ++i) {
				const struct fplinux_ui_session_document_choice
					*choice = &message->document_choices[i];
				struct json_object *entry =
					json_object_new_object();

				json_object_object_add(
					entry, "app_id",
					json_object_new_string(choice->app_id));
				json_object_object_add(
					entry, "title",
					json_object_new_string(choice->title));
				json_object_object_add(
					entry, "icon",
					json_object_new_string(choice->icon));
				json_object_array_add(choices, entry);
			}
			json_object_object_add(object, "document_choices",
					       choices);
		}
	}
	if (message->applied || message->storage_state ||
	    message->storage_error) {
		json_object_object_add(
			object, "applied",
			json_object_new_boolean(message->applied));
		add_string(object, "storage_state",
			   storage_names[message->storage_state]);
		json_object_object_add(
			object, "storage_error",
			json_object_new_int(message->storage_error));
	}
	if (message->type == FPLINUX_UI_SESSION_CONFIG) {
		json_object_object_add(object, "audio",
				       json_object_new_boolean(message->audio));
		json_object_object_add(
			object, "background_audio",
			json_object_new_boolean(message->background_audio));
		json_object_object_add(
			object, "vibration",
			json_object_new_boolean(message->vibration));
		json_object_object_add(
			object, "light_effects",
			json_object_new_int(message->light_effects));
	}
	if (message->type == FPLINUX_UI_SESSION_LIGHT_EFFECT) {
		struct json_object *effect = json_object_new_object();

		json_object_object_add(
			effect, "resources",
			json_object_new_int(message->light_effect.resources));
		json_object_object_add(
			effect, "display_level",
			json_object_new_int(
				message->light_effect.display_level));
		json_object_object_add(
			effect, "keypad_on",
			json_object_new_boolean(
				message->light_effect.keypad_on));
		json_object_object_add(object, "light_effect", effect);
		json_object_object_add(object, "serial",
				       json_object_new_int64(message->serial));
	}
	if (message->type == FPLINUX_UI_SESSION_VIBRATE) {
		struct json_object *pattern = json_object_new_array();

		for (unsigned int i = 0; i < message->pattern_count; ++i)
			json_object_array_add(
				pattern,
				json_object_new_int64(message->pattern[i]));
		json_object_object_add(object, "pattern", pattern);
		json_object_object_add(
			object, "repeat",
			json_object_new_boolean(message->repeat));
	}
	if ((message->type == FPLINUX_UI_SESSION_EXIT ||
	     message->type == FPLINUX_UI_SESSION_ACTIVE ||
	     message->type == FPLINUX_UI_SESSION_ACTIVATION ||
	     message->type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
	     message->type == FPLINUX_UI_SESSION_ACTIVE_ACK ||
	     message->type == FPLINUX_UI_SESSION_REDRAW_ACK ||
	     message->type == FPLINUX_UI_SESSION_CLOSE_ACK ||
	     message->type == FPLINUX_UI_SESSION_SETTING ||
	     message->type == FPLINUX_UI_SESSION_BLUETOOTH_ACTION ||
	     message->type == FPLINUX_UI_SESSION_ERROR ||
	     message->type == FPLINUX_UI_SESSION_CLOCK ||
	     message->type == FPLINUX_UI_SESSION_PREPARE_CLOSE ||
	     message->type == FPLINUX_UI_SESSION_CLOSE_REPLY) &&
	    message->serial)
		json_object_object_add(object, "serial",
				       json_object_new_int64(message->serial));
	add_string(object, "mode", mode_names[message->mode]);
	add_string(object, "app_id", message->app_id);
	if (message->type == FPLINUX_UI_SESSION_ACTIVATION)
		add_string(object, "activation",
			   activation_names[message->activation]);
	if (message->type == FPLINUX_UI_SESSION_LIFECYCLE)
		add_string(object, "lifecycle",
			   lifecycle_names[message->lifecycle]);
	if (message->type == FPLINUX_UI_SESSION_ACTIVATE ||
	    message->type == FPLINUX_UI_SESSION_CLOSE_TASK ||
	    (message->type == FPLINUX_UI_SESSION_ACTIVE && message->token))
		json_object_object_add(object, "token",
				       json_object_new_int64(message->token));
	if (message->type == FPLINUX_UI_SESSION_TASKS && message->has_tasks) {
		struct json_object *tasks = json_object_new_array();

		for (unsigned int i = 0; i < message->task_count; ++i) {
			const struct fplinux_ui_session_task *task =
				&message->tasks[i];
			struct json_object *entry = json_object_new_object();

			add_string(entry, "app_id", task->app_id);
			add_string(entry, "title", task->title);
			add_string(entry, "lifecycle",
				   lifecycle_names[task->lifecycle]);
			add_string(entry, "playback",
				   playback_names[task->playback]);
			json_object_object_add(
				entry, "token",
				json_object_new_int64(task->token));
			json_object_array_add(tasks, entry);
		}
		json_object_object_add(object, "tasks", tasks);
	}
	add_string(object, "active_id", message->active_id);
	add_string(object, "selected_id", message->selected_id);
	add_string(object, "title", message->title);
	add_string(object, "left", message->left);
	add_string(object, "centre", message->centre);
	add_string(object, "right", message->right);
	add_string(object, "reason", message->reason);
	add_string(object, "locale", message->locale);
	if (message->type == FPLINUX_UI_SESSION_CONFIG ||
	    message->type == FPLINUX_UI_SESSION_ACTIVE)
		add_string(object, "input_locale", message->input_locale);
	if (message->setting.kind)
		add_setting(object, message);
	else if (launcher) {
		if (launcher_view)
			add_string(object, "launcher_view",
				   launcher_view_names[launcher_view]);
		if (launcher_catalogue)
			add_string(
				object, "launcher_catalogue",
				launcher_catalogue_names[launcher_catalogue]);
	} else if (message->type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
		add_string(object, "value",
			   close_response_names[close_response]);
	} else {
		add_string(object, "name", message->name);
		add_string(object, "value", message->value);
	}
	add_string(object, "notice", message->notice);
	add_string(object, "display", display_names[message->display]);
	add_string(object, "theme", theme_names[message->theme]);
	if (message->type == FPLINUX_UI_SESSION_LAUNCH || message->has_frame) {
		json_object_object_add(object, "width",
				       json_object_new_int(message->width));
		json_object_object_add(object, "height",
				       json_object_new_int(message->height));
	}
	json_object_object_add(object, "screen_width",
			       json_object_new_int(message->screen_width));
	json_object_object_add(object, "screen_height",
			       json_object_new_int(message->screen_height));
	json_object_object_add(object, "locked",
			       json_object_new_boolean(message->locked));
	json_object_object_add(object, "visible",
			       json_object_new_boolean(message->visible));
	json_object_object_add(object, "enabled",
			       json_object_new_boolean(message->enabled));
	if (message->type == FPLINUX_UI_SESSION_ACTIVE)
		json_object_object_add(
			object, "tasks_view",
			json_object_new_boolean(message->tasks_view));
	if (message->type == FPLINUX_UI_SESSION_ACTIVE)
		json_object_object_add(
			object, "background_playing",
			json_object_new_boolean(message->background_playing));
	json_object_object_add(object, "battery",
			       json_object_new_int(message->battery));
	json_object_object_add(object, "charging",
			       json_object_new_boolean(message->charging));
	packet = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	length = strlen(packet);
	/* The receiver rejects a packet that fills its buffer; fail here instead. */
	if (length >= FPLINUX_UI_SESSION_PACKET_BYTES) {
		json_object_put(object);
		errno = EMSGSIZE;
		return false;
	}
	do {
		if (message->has_frame) {
			union {
				struct cmsghdr alignment;
				char bytes[CMSG_SPACE(sizeof(int))];
			} control = { 0 };
			struct iovec iov = { .iov_base = (void *)packet,
					     .iov_len = length };
			struct msghdr header = {
				.msg_iov = &iov,
				.msg_iovlen = 1,
				.msg_control = control.bytes,
				.msg_controllen = sizeof(control.bytes),
			};
			struct cmsghdr *rights = CMSG_FIRSTHDR(&header);

			rights->cmsg_level = SOL_SOCKET;
			rights->cmsg_type = SCM_RIGHTS;
			rights->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(rights), &message->frame_fd,
			       sizeof(int));
			sent = sendmsg(fd, &header, MSG_NOSIGNAL);
		} else {
			sent = send(fd, packet, length, MSG_NOSIGNAL);
		}
	} while (sent < 0 && errno == EINTR);
	error = sent < 0 ? errno : EIO;
	json_object_put(object);
	if (sent >= 0 && (size_t)sent == length)
		return true;
	/*
	 * There is no retransmission queue for this ordered control protocol.
	 * Losing a state update would leave the client out of
	 * sync, so a failed send ends the connection, also for EAGAIN. The
	 * owner still closes the descriptor through its usual disconnect path.
	 */
	fprintf(stderr,
		"fplinux-ui: control send failed (%s): %s; disconnecting\n",
		message_names[message->type], strerror(error));
	shutdown(fd, SHUT_RDWR);
	errno = error;
	return false;
}

int fplinux_ui_session_receive(int fd,
			       struct fplinux_ui_session_message *message)
{
	char packet[FPLINUX_UI_SESSION_PACKET_BYTES];
	struct json_tokener *tokener;
	struct json_object *object;
	struct json_object *field;
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(SESSION_MAX_FDS * sizeof(int))];
	} control;
	struct iovec iov = { .iov_base = packet, .iov_len = sizeof(packet) };
	struct msghdr header;
	ssize_t received;
	int frame_fd = -1;
	int frame_flags;
	unsigned int fd_count = 0;
	bool ancillary_valid = true;
	unsigned int type = ARRAY_SIZE(message_names);
	unsigned int mode = FPLINUX_UI_SESSION_LAUNCHER;
	unsigned int display = FPLINUX_UI_SESSION_DISPLAY_ON;
	unsigned int theme = FPLINUX_UI_SESSION_THEME_LIGHT;
	bool valid;

	do {
		memset(&header, 0, sizeof(header));
		header.msg_iov = &iov;
		header.msg_iovlen = 1;
		header.msg_control = control.bytes;
		header.msg_controllen = sizeof(control.bytes);
		received = recvmsg(fd, &header, MSG_TRUNC | MSG_CMSG_CLOEXEC);
	} while (received < 0 && errno == EINTR);
	if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	if (received < 0)
		return -1;
	memset(message, 0, sizeof(*message));
	/* musl's CMSG_NXTHDR() compares a size with a pointer difference. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
	for (struct cmsghdr *item = CMSG_FIRSTHDR(&header); item;
	     item = CMSG_NXTHDR(&header, item)) {
		size_t available = (char *)header.msg_control +
				   header.msg_controllen - (char *)item;
		size_t bytes;

		if (item->cmsg_len < CMSG_LEN(0) ||
		    item->cmsg_len > available) {
			ancillary_valid = false;
			break;
		}
		if (item->cmsg_level != SOL_SOCKET ||
		    item->cmsg_type != SCM_RIGHTS) {
			ancillary_valid = false;
			continue;
		}
		bytes = item->cmsg_len - CMSG_LEN(0);
		if (!bytes || bytes % sizeof(int))
			ancillary_valid = false;
		for (size_t offset = 0; offset + sizeof(int) <= bytes;
		     offset += sizeof(int)) {
			int attached;

			memcpy(&attached, (char *)CMSG_DATA(item) + offset,
			       sizeof(attached));
			++fd_count;
			if (frame_fd < 0)
				frame_fd = attached;
			else
				close(attached);
		}
	}
#pragma GCC diagnostic pop
	frame_flags = fd_count ? fcntl(frame_fd, F_GETFD) : 0;
	if (received == 0 || (size_t)received >= sizeof(packet) ||
	    (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || !ancillary_valid ||
	    fd_count > 1 ||
	    (fd_count && (frame_flags < 0 || !(frame_flags & FD_CLOEXEC)))) {
		if (frame_fd >= 0)
			close(frame_fd);
		errno = EPROTO;
		return -1;
	}
	tokener = json_tokener_new();
	if (!tokener) {
		if (frame_fd >= 0)
			close(frame_fd);
		return -1;
	}
	/*
	 * json-c 0.18's strict mode rejects every non-ASCII byte in a string, so
	 * Cyrillic labels would drop the connection. Field types and names are
	 * validated below; the tokener only has to check UTF-8.
	 */
	json_tokener_set_flags(tokener, JSON_TOKENER_VALIDATE_UTF8);
	object = json_tokener_parse_ex(tokener, packet, received);
	valid = json_tokener_get_error(tokener) == json_tokener_success &&
		json_tokener_get_parse_end(tokener) == (size_t)received &&
		json_object_is_type(object, json_type_object);
	json_tokener_free(tokener);
	valid = valid && has_only_known_fields(object) &&
		get_choice(object, "type", message_names,
			   ARRAY_SIZE(message_names), ARRAY_SIZE(message_names),
			   &type) &&
		type < ARRAY_SIZE(message_names) &&
		get_choice(object, "mode", mode_names, ARRAY_SIZE(mode_names),
			   FPLINUX_UI_SESSION_LAUNCHER, &mode) &&
		get_choice(object, "display", display_names,
			   ARRAY_SIZE(display_names),
			   FPLINUX_UI_SESSION_DISPLAY_ON, &display) &&
		get_choice(object, "theme", theme_names,
			   ARRAY_SIZE(theme_names),
			   FPLINUX_UI_SESSION_THEME_LIGHT, &theme) &&
		get_string(object, "app_id", message->app_id,
			   sizeof(message->app_id)) &&
		get_string(object, "active_id", message->active_id,
			   sizeof(message->active_id)) &&
		get_string(object, "selected_id", message->selected_id,
			   sizeof(message->selected_id)) &&
		get_string(object, "title", message->title,
			   sizeof(message->title)) &&
		get_string(object, "left", message->left,
			   sizeof(message->left)) &&
		get_string(object, "centre", message->centre,
			   sizeof(message->centre)) &&
		get_string(object, "right", message->right,
			   sizeof(message->right)) &&
		get_string(object, "reason", message->reason,
			   sizeof(message->reason)) &&
		get_string(object, "locale", message->locale,
			   sizeof(message->locale)) &&
		get_string(object, "input_locale", message->input_locale,
			   sizeof(message->input_locale)) &&
		input_locale_valid(message->input_locale) &&
		get_string(object, "name", message->name,
			   sizeof(message->name)) &&
		(type == FPLINUX_UI_SESSION_CLOSE_REPLY ?
			 get_close_response(object, message) :
			 get_string(object, "value", message->value,
				    sizeof(message->value))) &&
		get_string(object, "notice", message->notice,
			   sizeof(message->notice)) &&
		get_int(object, "width", &message->width) &&
		get_int(object, "height", &message->height) &&
		get_int(object, "screen_width", &message->screen_width) &&
		get_int(object, "screen_height", &message->screen_height) &&
		get_bool(object, "locked", &message->locked) &&
		get_bool(object, "visible", &message->visible) &&
		get_bool(object, "enabled", &message->enabled) &&
		get_bool(object, "tasks_view", &message->tasks_view) &&
		get_bool(object, "background_playing",
			 &message->background_playing) &&
		get_int(object, "battery", &message->battery) &&
		message->battery >= -1 && message->battery <= 100 &&
		get_bool(object, "charging", &message->charging);
	if (valid && json_object_object_get_ex(object, "input_locale", &field))
		valid = type == FPLINUX_UI_SESSION_CONFIG ||
			type == FPLINUX_UI_SESSION_ACTIVE;
	if (valid && json_object_object_get_ex(object, "tasks_view", &field))
		valid = type == FPLINUX_UI_SESSION_ACTIVE;
	if (valid &&
	    json_object_object_get_ex(object, "background_playing", &field))
		valid = type == FPLINUX_UI_SESSION_ACTIVE;
	if (valid)
		valid = get_task_fields(object, type, message);
	if (valid)
		valid = get_clock(object, type, packet, (size_t)received,
				  message->clock_data);
	if (valid)
		valid = get_clock_presentation(object, type, message);
	if (valid)
		valid = get_media_fields(object, type, message);
	if (valid)
		valid = get_light_fields(object, type, message);
	if (valid)
		valid = get_storage_fields(object, type, message);
	if (valid)
		valid = get_document_fields(object, type, message);
	if (valid)
		valid = get_volume_fields(object, type, message);
	if (valid)
		valid = get_system_levels(object, type,
					  &message->system_levels);
	if (valid)
		valid = get_setting(object, type, message) &&
			get_launcher_view(object, type, message);
	if (valid && json_object_object_get_ex(object, "serial", &field)) {
		int64_t serial = json_object_get_int64(field);

		valid = json_object_is_type(field, json_type_int) &&
			serial >= 0 && serial <= UINT32_MAX;
		if (valid)
			message->serial = (uint32_t)serial;
	}
	message->type = type;
	message->mode = mode;
	message->display = display;
	message->theme = theme;
	if (valid)
		valid = clock_presentation_valid(message);
	if (type == FPLINUX_UI_SESSION_STORAGE_RESULT)
		message->volume_result.request_id = message->serial;
	if (valid)
		valid = document_fields_valid(message);
	if (valid)
		valid = volume_fields_valid(message);
	if (valid)
		valid = light_fields_valid(message);
	if (valid)
		valid = get_wifi_fields(object, message) &&
			wifi_fields_valid(message);
	if (valid)
		valid = get_bluetooth_prompt(object, message) &&
			bluetooth_fields_valid(message);
	json_object_put(object);
	if (valid && (type == FPLINUX_UI_SESSION_ACTIVE_ACK ||
		      type == FPLINUX_UI_SESSION_ACTIVATION ||
		      type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
		      type == FPLINUX_UI_SESSION_REDRAW_ACK ||
		      type == FPLINUX_UI_SESSION_CLOSE_ACK ||
		      type == FPLINUX_UI_SESSION_PREPARE_CLOSE ||
		      type == FPLINUX_UI_SESSION_CLOSE_REPLY))
		valid = message->serial != 0;
	if (valid && fd_count)
		valid = frame_message_valid(message) &&
			frame_dimensions_valid(message);
	if (!valid) {
		if (frame_fd >= 0)
			close(frame_fd);
		errno = EPROTO;
		return -1;
	}
	if (fd_count) {
		message->has_frame = true;
		message->frame_fd = frame_fd;
	}
	return 1;
}

int fplinux_ui_session_connect_at(const char *path, const char *app_id)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct fplinux_ui_session_message hello = {
		.type = FPLINUX_UI_SESSION_HELLO,
	};
	int fd;
	int flags;
	int error;

	if (strlen(path) >= sizeof(address.sun_path) ||
	    strlen(app_id) >= sizeof(hello.app_id)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(address.sun_path, path);
	strcpy(hello.app_id, app_id);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    !fplinux_ui_session_send(fd, &hello))
		goto fail;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto fail;
	return fd;
fail:
	error = errno;
	close(fd);
	errno = error;
	return -1;
}

int fplinux_ui_session_connect(const char *app_id)
{
	const char *path = getenv("FPLINUX_UI_SESSION_SOCKET");

	return fplinux_ui_session_connect_at(
		path && path[0] ? path : FPLINUX_UI_SESSION_SOCKET, app_id);
}
