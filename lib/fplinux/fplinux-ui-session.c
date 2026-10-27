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
	"locale",
	"input_locale",
	"name",
	"value",
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

static bool get_clock(struct json_object *object, unsigned int type, char *data)
{
	struct json_object *field;

	if (json_object_object_get_ex(object, "clock", &field) &&
	    type != FPLINUX_UI_SESSION_CLOCK &&
	    type != FPLINUX_UI_SESSION_ACTIVE)
		return false;
	return get_string(object, "clock", data,
			  FPLINUX_UI_SESSION_CLOCK_BYTES);
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
	    (unsigned int)task->lifecycle >= ARRAY_SIZE(lifecycle_names))
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

		if (!json_object_is_type(entry, json_type_object) ||
		    json_object_object_length(entry) != 4 ||
		    !get_string(entry, "app_id", task->app_id,
				sizeof(task->app_id)) ||
		    !get_string(entry, "title", task->title,
				sizeof(task->title)) ||
		    !get_lifecycle(entry, &task->lifecycle) ||
		    !get_token(entry, &task->token))
			return false;
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

static bool close_response_valid(const char *value)
{
	return !strcmp(value, "ready") || !strcmp(value, "cancel") ||
	       !strcmp(value, "wait");
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
		"id",	     "mount_id", "state", "protection",
		"read_only", "label",	 "path",
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
		json_object_object_add(entry, "label",
				       json_object_new_string(volume->label));
		json_object_object_add(entry, "path",
				       json_object_new_string(volume->path));
		json_object_array_add(array, entry);
	}
	json_object_object_add(object, "volumes", array);
}

static bool is_document_type(unsigned int type)
{
	return type >= FPLINUX_UI_SESSION_OPEN_FILE &&
	       type <= FPLINUX_UI_SESSION_DOCUMENT_RESULT;
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
	if (m->type != FPLINUX_UI_SESSION_OPEN_FILE &&
	    m->type != FPLINUX_UI_SESSION_DOCUMENT && m->path[0])
		return false;
	if (m->type == FPLINUX_UI_SESSION_OPEN_FILE ||
	    m->type == FPLINUX_UI_SESSION_DOCUMENT)
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
	     type != FPLINUX_UI_SESSION_OPEN_FILE &&
	     type != FPLINUX_UI_SESSION_DOCUMENT) ||
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

static bool get_media_fields(struct json_object *object, unsigned int type,
			     struct fplinux_ui_session_message *message)
{
	struct json_object *field;
	struct json_object *pattern;
	size_t count;

	if ((json_object_object_get_ex(object, "audio", &field) ||
	     json_object_object_get_ex(object, "vibration", &field)) &&
	    type != FPLINUX_UI_SESSION_CONFIG)
		return false;
	if (!get_bool(object, "audio", &message->audio) ||
	    !get_bool(object, "vibration", &message->vibration))
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
	    !volume_fields_valid(message) ||
	    ((message->audio || message->vibration) &&
	     message->type != FPLINUX_UI_SESSION_CONFIG) ||
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
	     !close_response_valid(message->value))) {
		errno = EINVAL;
		return false;
	}
	object = json_object_new_object();
	if (!object)
		return false;
	add_string(object, "clock", message->clock_data);
	if (message->type == FPLINUX_UI_SESSION_ACTIVE)
		add_clock_presentation(object, &message->clock_presentation);
	add_string(object, "type", message_names[message->type]);
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
			object, "vibration",
			json_object_new_boolean(message->vibration));
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
	if ((message->type == FPLINUX_UI_SESSION_ACTIVE ||
	     message->type == FPLINUX_UI_SESSION_ACTIVATION ||
	     message->type == FPLINUX_UI_SESSION_ACTIVATION_ACK ||
	     message->type == FPLINUX_UI_SESSION_ACTIVE_ACK ||
	     message->type == FPLINUX_UI_SESSION_REDRAW_ACK ||
	     message->type == FPLINUX_UI_SESSION_CLOSE_ACK ||
	     message->type == FPLINUX_UI_SESSION_SETTING ||
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
	add_string(object, "name", message->name);
	add_string(object, "value", message->value);
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
		get_string(object, "value", message->value,
			   sizeof(message->value)) &&
		get_string(object, "notice", message->notice,
			   sizeof(message->notice)) &&
		get_int(object, "width", &message->width) &&
		get_int(object, "height", &message->height) &&
		get_int(object, "screen_width", &message->screen_width) &&
		get_int(object, "screen_height", &message->screen_height) &&
		get_bool(object, "locked", &message->locked) &&
		get_bool(object, "visible", &message->visible) &&
		get_bool(object, "enabled", &message->enabled) &&
		get_int(object, "battery", &message->battery) &&
		message->battery >= -1 && message->battery <= 100 &&
		get_bool(object, "charging", &message->charging);
	if (valid && json_object_object_get_ex(object, "input_locale", &field))
		valid = type == FPLINUX_UI_SESSION_CONFIG ||
			type == FPLINUX_UI_SESSION_ACTIVE;
	if (valid)
		valid = get_task_fields(object, type, message);
	if (valid)
		valid = get_clock(object, type, message->clock_data);
	if (valid)
		valid = get_clock_presentation(object, type, message);
	if (valid)
		valid = get_media_fields(object, type, message);
	if (valid)
		valid = get_storage_fields(object, type, message);
	if (valid)
		valid = get_document_fields(object, type, message);
	if (valid)
		valid = get_volume_fields(object, type, message);
	if (valid && type == FPLINUX_UI_SESSION_CLOSE_REPLY)
		valid = close_response_valid(message->value);
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
