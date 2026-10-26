/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-wire.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

static const char *const request_names[] = {
	"snapshot",
	"alarm-add",
	"alarm-edit",
	"alarm-enable",
	"alarm-delete",
	"dismiss",
	"snooze",
	"timer-set",
	"timer-start",
	"timer-pause",
	"timer-resume",
	"timer-stop",
	"timer-restart",
	"stopwatch-start",
	"stopwatch-pause",
	"stopwatch-reset",
	"stopwatch-lap",
	"world-add",
	"world-edit",
	"world-delete",
	"set-view",
	"set-snooze",
	"clear-missed",
	"save",
	"alarm-cancel-snooze",
};

const char *clock_wire_request_name(enum clock_request_op op)
{
	if ((unsigned int)op >=
	    sizeof(request_names) / sizeof(request_names[0]))
		return NULL;
	return request_names[op];
}

static bool member(struct json_object *object, const char *name,
		   enum json_type type, struct json_object **value)
{
	return object && json_object_get_type(object) == json_type_object &&
	       json_object_object_get_ex(object, name, value) && *value &&
	       json_object_get_type(*value) == type;
}

static bool shape(struct json_object *object, size_t members)
{
	return object && json_object_get_type(object) == json_type_object &&
	       (size_t)json_object_object_length(object) == members;
}

static bool unsigned_value(struct json_object *object, const char *name,
			   uint64_t maximum, uint64_t *value)
{
	struct json_object *number;
	if (!member(object, name, json_type_int, &number))
		return false;
	/* json-c can store uint64 values above INT64_MAX; signed extraction would
	 * saturate. The lexical sign distinguishes negative values without loss. */
	if (json_object_to_json_string_ext(number, JSON_C_TO_STRING_PLAIN)[0] ==
	    '-')
		return false;
	*value = json_object_get_uint64(number);
	return *value <= maximum;
}

static bool signed_value(struct json_object *object, const char *name,
			 int64_t minimum, int64_t maximum, int64_t *value)
{
	struct json_object *number;
	if (!member(object, name, json_type_int, &number))
		return false;
	if (json_object_to_json_string_ext(number, JSON_C_TO_STRING_PLAIN)[0] !=
		    '-' &&
	    json_object_get_uint64(number) > INT64_MAX)
		return false;
	*value = json_object_get_int64(number);
	return *value >= minimum && *value <= maximum;
}

static bool boolean_value(struct json_object *object, const char *name,
			  bool *value)
{
	struct json_object *boolean;
	if (!member(object, name, json_type_boolean, &boolean))
		return false;
	*value = json_object_get_boolean(boolean) != 0;
	return true;
}

static bool offset_value(struct json_object *object, int *offset_minutes)
{
	int64_t value;

	if (!signed_value(object, "offset_minutes", INT_MIN, INT_MAX, &value) ||
	    !clock_model_world_offset_valid((int)value))
		return false;
	*offset_minutes = (int)value;
	return true;
}

static bool text_value(struct json_object *object, const char *name,
		       char text[CLOCK_TEXT_BYTES + 1], bool allow_empty)
{
	struct json_object *string;
	const char *bytes;
	size_t length;
	if (!member(object, name, json_type_string, &string))
		return false;
	bytes = json_object_get_string(string);
	length = (size_t)json_object_get_string_len(string);
	if (clock_model_validate_text(bytes, length, allow_empty) != CLOCK_OK)
		return false;
	memcpy(text, bytes, length);
	text[length] = '\0';
	return true;
}

static bool literal_value(struct json_object *object, const char *name,
			  const char *literal)
{
	struct json_object *string;
	size_t length = strlen(literal);
	return member(object, name, json_type_string, &string) &&
	       (size_t)json_object_get_string_len(string) == length &&
	       memcmp(json_object_get_string(string), literal, length) == 0;
}

/* json-c parses the document and validates UTF-8, but replaces duplicate keys
 * and saturates integers. Check original tokens alongside its tree to retain
 * exact identities and the strict grammar of this domain's objects, arrays,
 * strings, integer fields and booleans. No second representation is built. */
struct lexical_check {
	const char *text;
	size_t length;
	size_t position;
};

static void skip_space(struct lexical_check *check)
{
	while (check->position < check->length) {
		char byte = check->text[check->position];
		if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
			break;
		++check->position;
	}
}

static bool unicode_escape(const char *text, size_t length, size_t position,
			   unsigned int *codepoint)
{
	if (position > length || length - position < 4)
		return false;
	*codepoint = 0;
	for (size_t index = 0; index < 4; ++index) {
		unsigned char byte = (unsigned char)text[position + index];
		unsigned int digit;
		if (byte >= '0' && byte <= '9')
			digit = byte - '0';
		else if (byte >= 'a' && byte <= 'f')
			digit = byte - 'a' + 10;
		else if (byte >= 'A' && byte <= 'F')
			digit = byte - 'A' + 10;
		else
			return false;
		*codepoint = *codepoint * 16 + digit;
	}
	return true;
}

static bool scan_string(struct lexical_check *check, size_t *start,
			size_t *length)
{
	*start = check->position;
	if (check->position >= check->length ||
	    check->text[check->position++] != '"')
		return false;
	while (check->position < check->length) {
		char byte = check->text[check->position++];
		if ((unsigned char)byte < 0x20)
			return false;
		if (byte == '\\') {
			if (check->position >= check->length)
				return false;
			byte = check->text[check->position++];
			if (byte != '"' && byte != '\\' && byte != '/' &&
			    byte != 'b' && byte != 'f' && byte != 'n' &&
			    byte != 'r' && byte != 't' && byte != 'u')
				return false;
			if (byte == 'u') {
				unsigned int codepoint, low;
				if (!unicode_escape(check->text, check->length,
						    check->position,
						    &codepoint))
					return false;
				check->position += 4;
				if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
					return false;
				if (codepoint >= 0xd800 &&
				    codepoint <= 0xdbff) {
					if (check->length - check->position <
						    6 ||
					    check->text[check->position] !=
						    '\\' ||
					    check->text[check->position + 1] !=
						    'u' ||
					    !unicode_escape(check->text,
							    check->length,
							    check->position + 2,
							    &low) ||
					    low < 0xdc00 || low > 0xdfff)
						return false;
					check->position += 6;
				}
			}
		} else if (byte == '"') {
			*length = check->position - *start;
			return true;
		}
	}
	return false;
}

static bool scan_tree(struct lexical_check *check, struct json_object *object,
		      unsigned int depth)
{
	if (depth > 16)
		return false;
	skip_space(check);
	if (check->position >= check->length)
		return false;
	char byte = check->text[check->position];
	if (byte == '{') {
		size_t count = 0;
		if (!object || json_object_get_type(object) != json_type_object)
			return false;
		++check->position;
		skip_space(check);
		while (check->position < check->length &&
		       check->text[check->position] != '}') {
			size_t start, length;
			struct json_object *key, *value = NULL;
			struct json_tokener *tokener;
			if (!scan_string(check, &start, &length))
				return false;
			tokener = json_tokener_new_ex(4);
			if (!tokener)
				return false;
			key = json_tokener_parse_ex(
				tokener, check->text + start, (int)length);
			bool valid =
				key &&
				json_tokener_get_error(tokener) ==
					json_tokener_success &&
				json_object_get_type(key) == json_type_string &&
				strlen(json_object_get_string(key)) ==
					(size_t)json_object_get_string_len(
						key) &&
				json_object_object_get_ex(
					object, json_object_get_string(key),
					&value);
			if (key)
				json_object_put(key);
			json_tokener_free(tokener);
			if (!valid)
				return false;
			skip_space(check);
			if (check->position >= check->length ||
			    check->text[check->position++] != ':' ||
			    !scan_tree(check, value, depth + 1))
				return false;
			++count;
			skip_space(check);
			if (check->position < check->length &&
			    check->text[check->position] == ',') {
				++check->position;
				skip_space(check);
				if (check->position >= check->length ||
				    check->text[check->position] == '}')
					return false;
			} else {
				break;
			}
		}
		if (check->position >= check->length ||
		    check->text[check->position++] != '}')
			return false;
		return count == (size_t)json_object_object_length(object);
	}
	if (byte == '[') {
		size_t count = 0;
		if (!object || json_object_get_type(object) != json_type_array)
			return false;
		++check->position;
		skip_space(check);
		while (check->position < check->length &&
		       check->text[check->position] != ']') {
			if (count >= json_object_array_length(object) ||
			    !scan_tree(check,
				       json_object_array_get_idx(object,
								 count++),
				       depth + 1))
				return false;
			skip_space(check);
			if (check->position < check->length &&
			    check->text[check->position] == ',') {
				++check->position;
				skip_space(check);
				if (check->position >= check->length ||
				    check->text[check->position] == ']')
					return false;
			} else {
				break;
			}
		}
		if (check->position >= check->length ||
		    check->text[check->position++] != ']')
			return false;
		return count == json_object_array_length(object);
	}
	if (byte == '"') {
		size_t start, length;
		return object &&
		       json_object_get_type(object) == json_type_string &&
		       scan_string(check, &start, &length);
	}
	size_t start = check->position;
	while (check->position < check->length) {
		byte = check->text[check->position];
		if (byte == ',' || byte == '}' || byte == ']' || byte == ' ' ||
		    byte == '\t' || byte == '\n' || byte == '\r')
			break;
		++check->position;
	}
	size_t length = check->position - start;
	if (object && json_object_get_type(object) == json_type_int) {
		char number[32], *end;
		if (!length || length >= sizeof(number))
			return false;
		memcpy(number, check->text + start, length);
		number[length] = '\0';
		size_t digit = number[0] == '-' ? 1 : 0;
		if (digit >= length || number[digit] < '0' ||
		    number[digit] > '9' ||
		    (number[digit] == '0' && digit + 1 != length))
			return false;
		for (size_t index = digit; index < length; ++index)
			if (number[index] < '0' || number[index] > '9')
				return false;
		errno = 0;
		if (number[0] == '-')
			(void)strtoll(number, &end, 10);
		else
			(void)strtoull(number, &end, 10);
		if (errno == ERANGE || *end != '\0')
			return false;
		return true;
	}
	if (object && json_object_get_type(object) == json_type_boolean)
		return (length == 4 &&
			memcmp(check->text + start, "true", 4) == 0) ||
		       (length == 5 &&
			memcmp(check->text + start, "false", 5) == 0);
	return false;
}

static struct json_object *parse_object(const char *json, size_t length)
{
	struct json_tokener *tokener;
	struct json_object *object;
	struct lexical_check check = { json, length, 0 };
	if (!json || !length || length >= CLOCK_WIRE_CAPACITY ||
	    memchr(json, '\0', length))
		return NULL;
	tokener = json_tokener_new_ex(16);
	if (!tokener)
		return NULL;
	/* This json-c build's STRICT mode rejects valid raw multibyte characters.
	 * Use its UTF-8 validator and enforce domain JSON grammar in scan_tree. */
	json_tokener_set_flags(tokener, JSON_TOKENER_VALIDATE_UTF8);
	object = json_tokener_parse_ex(tokener, json, (int)length);
	bool valid = object &&
		     json_tokener_get_error(tokener) == json_tokener_success &&
		     json_object_get_type(object) == json_type_object &&
		     scan_tree(&check, object, 0);
	skip_space(&check);
	valid = valid && check.position == length;
	json_tokener_free(tokener);
	if (!valid && object) {
		json_object_put(object);
		object = NULL;
	}
	return object;
}

static enum clock_status write_object(struct json_object *object, char *json,
				      size_t capacity)
{
	const char *text;
	size_t length;
	if (!object || !json || !capacity)
		return CLOCK_INVALID;
	text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	if (!text)
		return CLOCK_INVALID;
	length = strlen(text);
	if (length >= capacity || length >= CLOCK_WIRE_CAPACITY)
		return CLOCK_FULL;
	memcpy(json, text, length + 1);
	return CLOCK_OK;
}

static void add_uint(struct json_object *object, const char *name,
		     uint64_t value)
{
	json_object_object_add(object, name, json_object_new_uint64(value));
}

static void add_int(struct json_object *object, const char *name, int64_t value)
{
	json_object_object_add(object, name, json_object_new_int64(value));
}

static void add_bool(struct json_object *object, const char *name, bool value)
{
	json_object_object_add(object, name, json_object_new_boolean(value));
}

static void add_text(struct json_object *object, const char *name,
		     const char *value)
{
	/* Bounded public DTOs must have a NUL inside their declared storage. */
	const char *end = memchr(value, '\0', CLOCK_TEXT_BYTES + 1);
	json_object_object_add(
		object, name,
		end ? json_object_new_string_len(value, (int)(end - value)) :
		      NULL);
}

static bool read_alarm_spec(struct json_object *object,
			    struct clock_alarm_spec *spec)
{
	uint64_t schedule, hour, minute, days;
	if (!shape(object, 7) ||
	    !text_value(object, "label", spec->label, true) ||
	    !unsigned_value(object, "schedule", CLOCK_ALARM_WEEKLY,
			    &schedule) ||
	    !signed_value(object, "once_utc", 0, CLOCK_WALL_MAX,
			  &spec->once_utc) ||
	    !unsigned_value(object, "hour", 23, &hour) ||
	    !unsigned_value(object, "minute", 59, &minute) ||
	    !unsigned_value(object, "repeat_days", 127, &days) ||
	    !boolean_value(object, "enabled", &spec->enabled))
		return false;
	spec->schedule = (enum clock_alarm_schedule)schedule;
	spec->hour = (unsigned int)hour;
	spec->minute = (unsigned int)minute;
	spec->repeat_days = (unsigned int)days;
	if (spec->schedule == CLOCK_ALARM_ONCE)
		return spec->once_utc >= CLOCK_WALL_MIN &&
		       spec->once_utc % 60 == 0 && !hour && !minute && !days;
	return !spec->once_utc && days;
}

static struct json_object *make_alarm_spec(const struct clock_alarm_spec *spec)
{
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	add_text(object, "label", spec->label);
	add_uint(object, "schedule", spec->schedule);
	add_int(object, "once_utc", spec->once_utc);
	add_uint(object, "hour", spec->hour);
	add_uint(object, "minute", spec->minute);
	add_uint(object, "repeat_days", spec->repeat_days);
	add_bool(object, "enabled", spec->enabled);
	return object;
}

static bool read_request(struct json_object *object,
			 struct clock_request *request)
{
	struct json_object *name, *alarm;
	uint64_t value;
	size_t count = 1;
	if (!member(object, "op", json_type_string, &name))
		return false;
	bool found = false;
	for (size_t index = 0;
	     index < sizeof(request_names) / sizeof(request_names[0]); ++index)
		if (literal_value(object, "op", request_names[index])) {
			request->op = (enum clock_request_op)index;
			found = true;
			break;
		}
	if (!found)
		return false;
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
	case CLOCK_REQUEST_ALARM_DELETE:
	case CLOCK_REQUEST_ALARM_CANCEL_SNOOZE:
	case CLOCK_REQUEST_WORLD_EDIT:
	case CLOCK_REQUEST_WORLD_DELETE:
		if (!unsigned_value(object, "id", UINT32_MAX, &value) || !value)
			return false;
		request->id = (uint32_t)value;
		++count;
		break;
	default:
		break;
	}
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
	case CLOCK_REQUEST_ALARM_DELETE:
		if (!boolean_value(object, "cancel_active",
				   &request->cancel_active))
			return false;
		++count;
		break;
	default:
		break;
	}
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_ADD:
	case CLOCK_REQUEST_ALARM_EDIT:
		if (!member(object, "alarm", json_type_object, &alarm) ||
		    !read_alarm_spec(alarm, &request->alarm))
			return false;
		++count;
		break;
	case CLOCK_REQUEST_ALARM_ENABLE:
		if (!boolean_value(object, "enabled", &request->enabled))
			return false;
		++count;
		break;
	case CLOCK_REQUEST_DISMISS:
	case CLOCK_REQUEST_SNOOZE:
		if (!unsigned_value(object, "event_id", UINT64_MAX,
				    &request->event_id) ||
		    !request->event_id)
			return false;
		++count;
		if (request->op == CLOCK_REQUEST_DISMISS)
			break;
		if (!unsigned_value(object, "minutes", 30, &value))
			return false;
		request->minutes = (unsigned int)value;
		++count;
		break;
	case CLOCK_REQUEST_TIMER_SET:
		if (!unsigned_value(object, "seconds", CLOCK_TIMER_MAX_SECONDS,
				    &value))
			return false;
		request->seconds = (unsigned int)value;
		++count;
		break;
	case CLOCK_REQUEST_WORLD_ADD:
	case CLOCK_REQUEST_WORLD_EDIT:
		if (!offset_value(object, &request->offset_minutes))
			return false;
		++count;
		break;
	case CLOCK_REQUEST_SET_VIEW:
		if (!unsigned_value(object, "view", CLOCK_VIEW_ANALOG, &value))
			return false;
		request->view = (enum clock_view)value;
		++count;
		break;
	case CLOCK_REQUEST_SET_SNOOZE:
		if (!unsigned_value(object, "minutes", 30, &value) || !value)
			return false;
		request->minutes = (unsigned int)value;
		++count;
		break;
	default:
		break;
	}
	return shape(object, count);
}

static struct json_object *make_request(const struct clock_request *request)
{
	const char *name = clock_wire_request_name(request->op);
	if (!name)
		return NULL;
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	json_object_object_add(object, "op", json_object_new_string(name));
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
	case CLOCK_REQUEST_ALARM_DELETE:
	case CLOCK_REQUEST_ALARM_CANCEL_SNOOZE:
	case CLOCK_REQUEST_WORLD_EDIT:
	case CLOCK_REQUEST_WORLD_DELETE:
		add_uint(object, "id", request->id);
		break;
	default:
		break;
	}
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
	case CLOCK_REQUEST_ALARM_DELETE:
		add_bool(object, "cancel_active", request->cancel_active);
		break;
	default:
		break;
	}
	switch (request->op) {
	case CLOCK_REQUEST_ALARM_ADD:
	case CLOCK_REQUEST_ALARM_EDIT:
		json_object_object_add(object, "alarm",
				       make_alarm_spec(&request->alarm));
		break;
	case CLOCK_REQUEST_ALARM_ENABLE:
		add_bool(object, "enabled", request->enabled);
		break;
	case CLOCK_REQUEST_DISMISS:
	case CLOCK_REQUEST_SNOOZE:
		add_uint(object, "event_id", request->event_id);
		if (request->op == CLOCK_REQUEST_SNOOZE)
			add_uint(object, "minutes", request->minutes);
		break;
	case CLOCK_REQUEST_TIMER_SET:
		add_uint(object, "seconds", request->seconds);
		break;
	case CLOCK_REQUEST_WORLD_ADD:
	case CLOCK_REQUEST_WORLD_EDIT:
		add_int(object, "offset_minutes", request->offset_minutes);
		break;
	case CLOCK_REQUEST_SET_VIEW:
		add_uint(object, "view", request->view);
		break;
	case CLOCK_REQUEST_SET_SNOOZE:
		add_uint(object, "minutes", request->minutes);
		break;
	default:
		break;
	}
	return object;
}

enum clock_status clock_wire_request_encode(const struct clock_request *request,
					    char *json, size_t capacity)
{
	struct clock_request checked = { 0 };
	struct json_object *object;
	if (!request)
		return CLOCK_INVALID;
	object = make_request(request);
	enum clock_status status =
		read_request(object, &checked) ?
			write_object(object, json, capacity) :
			CLOCK_INVALID;
	if (object)
		json_object_put(object);
	return status;
}

enum clock_status clock_wire_request_decode(const char *json, size_t length,
					    struct clock_request *request)
{
	struct clock_request decoded = { 0 };
	struct json_object *object;
	if (!request)
		return CLOCK_INVALID;
	object = parse_object(json, length);
	bool valid = read_request(object, &decoded);
	if (object)
		json_object_put(object);
	if (!valid)
		return CLOCK_INVALID;
	*request = decoded;
	return CLOCK_OK;
}

enum clock_status clock_wire_request_apply(struct clock_model *model,
					   const struct clock_request *request)
{
	struct clock_request checked = { 0 };
	struct json_object *object;
	if (!model || !request)
		return CLOCK_INVALID;
	object = make_request(request);
	bool valid = read_request(object, &checked);
	if (object)
		json_object_put(object);
	if (!valid)
		return CLOCK_INVALID;
	if ((checked.op == CLOCK_REQUEST_ALARM_ADD ||
	     checked.op == CLOCK_REQUEST_ALARM_EDIT) &&
	    checked.alarm.enabled &&
	    checked.alarm.schedule == CLOCK_ALARM_ONCE) {
		int64_t due;

		if (clock_model_alarm_next_due(&checked.alarm, model->wall_utc,
					       &due) != CLOCK_OK)
			return CLOCK_INVALID;
	}
	switch (checked.op) {
	case CLOCK_REQUEST_SNAPSHOT:
	case CLOCK_REQUEST_SAVE:
		return CLOCK_OK;
	case CLOCK_REQUEST_ALARM_ADD:
		return clock_model_alarm_add(model, &checked.alarm, NULL);
	case CLOCK_REQUEST_ALARM_EDIT:
		return clock_model_alarm_edit(model, checked.id, &checked.alarm,
					      checked.cancel_active);
	case CLOCK_REQUEST_ALARM_ENABLE:
		return clock_model_alarm_enable(model, checked.id,
						checked.enabled,
						checked.cancel_active);
	case CLOCK_REQUEST_ALARM_DELETE:
		return clock_model_alarm_delete(model, checked.id,
						checked.cancel_active);
	case CLOCK_REQUEST_ALARM_CANCEL_SNOOZE:
		return clock_model_alarm_cancel_snooze(model, checked.id);
	case CLOCK_REQUEST_DISMISS:
		return clock_model_dismiss(model, checked.event_id);
	case CLOCK_REQUEST_SNOOZE:
		return clock_model_snooze(model, checked.event_id,
					  checked.minutes);
	case CLOCK_REQUEST_TIMER_SET:
		return clock_model_timer_set(model, checked.seconds);
	case CLOCK_REQUEST_TIMER_START:
		return clock_model_timer_start(model);
	case CLOCK_REQUEST_TIMER_PAUSE:
		return clock_model_timer_pause(model);
	case CLOCK_REQUEST_TIMER_RESUME:
		return clock_model_timer_resume(model);
	case CLOCK_REQUEST_TIMER_STOP:
		return clock_model_timer_stop(model);
	case CLOCK_REQUEST_TIMER_RESTART:
		return clock_model_timer_restart(model);
	case CLOCK_REQUEST_STOPWATCH_START:
		return clock_model_stopwatch_start(model);
	case CLOCK_REQUEST_STOPWATCH_PAUSE:
		return clock_model_stopwatch_pause(model);
	case CLOCK_REQUEST_STOPWATCH_RESET:
		return clock_model_stopwatch_reset(model);
	case CLOCK_REQUEST_STOPWATCH_LAP:
		return clock_model_stopwatch_lap(model);
	case CLOCK_REQUEST_WORLD_ADD:
		return clock_model_world_add(model, checked.offset_minutes,
					     NULL);
	case CLOCK_REQUEST_WORLD_EDIT:
		return clock_model_world_edit(model, checked.id,
					      checked.offset_minutes);
	case CLOCK_REQUEST_WORLD_DELETE:
		return clock_model_world_delete(model, checked.id);
	case CLOCK_REQUEST_SET_VIEW:
		return clock_model_set_view(model, checked.view);
	case CLOCK_REQUEST_SET_SNOOZE:
		return clock_model_set_snooze(model, checked.minutes);
	case CLOCK_REQUEST_CLEAR_MISSED:
		clock_model_clear_missed(model);
		return CLOCK_OK;
	}
	return CLOCK_INVALID;
}

static bool wall_value(struct json_object *object, const char *name,
		       int64_t maximum, bool allow_zero, int64_t *value)
{
	return signed_value(object, name, allow_zero ? 0 : CLOCK_WALL_MIN,
			    maximum, value) &&
	       (*value >= CLOCK_WALL_MIN || (allow_zero && !*value));
}

static bool read_event(struct json_object *object, struct clock_event *event)
{
	uint64_t kind, source, generation, reason;
	if (!shape(object, 10) ||
	    !unsigned_value(object, "id", UINT64_MAX, &event->id) ||
	    !unsigned_value(object, "kind", CLOCK_EVENT_TIMER, &kind) ||
	    !unsigned_value(object, "source_id", UINT32_MAX, &source) ||
	    !unsigned_value(object, "generation", UINT32_MAX, &generation) ||
	    !text_value(object, "label", event->label, true) ||
	    !wall_value(object, "due_wall_utc", CLOCK_WALL_MAX, true,
			&event->due_wall_utc) ||
	    !unsigned_value(object, "due_monotonic_ms", UINT64_MAX,
			    &event->due_monotonic_ms) ||
	    !unsigned_value(object, "ring_started_ms", UINT64_MAX,
			    &event->ring_started_ms) ||
	    !boolean_value(object, "snoozed", &event->snoozed) ||
	    !unsigned_value(object, "missed_reason", CLOCK_MISSED_EXPIRED,
			    &reason))
		return false;
	event->kind = (enum clock_event_kind)kind;
	event->source_id = (uint32_t)source;
	event->generation = (uint32_t)generation;
	event->missed_reason = (enum clock_missed_reason)reason;
	if (event->kind == CLOCK_EVENT_NONE)
		return !event->id && !source && !generation &&
		       !event->label[0] && !event->due_wall_utc &&
		       !event->due_monotonic_ms && !event->ring_started_ms &&
		       !event->snoozed && !reason;
	return event->id && generation &&
	       event->due_wall_utc >= CLOCK_WALL_MIN &&
	       (event->kind == CLOCK_EVENT_ALARM ? source != 0 : source == 0) &&
	       (event->kind == CLOCK_EVENT_ALARM || !event->snoozed);
}

static struct json_object *make_event(const struct clock_event *event)
{
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	add_uint(object, "id", event->id);
	add_uint(object, "kind", event->kind);
	add_uint(object, "source_id", event->source_id);
	add_uint(object, "generation", event->generation);
	add_text(object, "label", event->label);
	add_int(object, "due_wall_utc", event->due_wall_utc);
	add_uint(object, "due_monotonic_ms", event->due_monotonic_ms);
	add_uint(object, "ring_started_ms", event->ring_started_ms);
	add_bool(object, "snoozed", event->snoozed);
	add_uint(object, "missed_reason", event->missed_reason);
	return object;
}

static bool read_alarm(struct json_object *object, struct clock_alarm *alarm)
{
	struct json_object *spec;
	uint64_t id, generation;
	if (!shape(object, 7) ||
	    !unsigned_value(object, "id", UINT32_MAX, &id) || !id ||
	    !unsigned_value(object, "generation", UINT32_MAX, &generation) ||
	    !generation || !member(object, "spec", json_type_object, &spec) ||
	    !read_alarm_spec(spec, &alarm->spec) ||
	    !wall_value(object, "next_due_utc", CLOCK_WALL_MAX, true,
			&alarm->next_due_utc) ||
	    !wall_value(object, "last_due_utc", CLOCK_WALL_MAX, true,
			&alarm->last_due_utc) ||
	    !boolean_value(object, "snoozed", &alarm->snoozed) ||
	    !unsigned_value(object, "snooze_deadline_ms", UINT64_MAX,
			    &alarm->snooze_deadline_ms))
		return false;
	alarm->id = (uint32_t)id;
	alarm->generation = (uint32_t)generation;
	return true;
}

static struct json_object *make_alarm(const struct clock_alarm *alarm)
{
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	add_uint(object, "id", alarm->id);
	add_uint(object, "generation", alarm->generation);
	json_object_object_add(object, "spec", make_alarm_spec(&alarm->spec));
	add_int(object, "next_due_utc", alarm->next_due_utc);
	add_int(object, "last_due_utc", alarm->last_due_utc);
	add_bool(object, "snoozed", alarm->snoozed);
	add_uint(object, "snooze_deadline_ms", alarm->snooze_deadline_ms);
	return object;
}

static bool array_value(struct json_object *object, const char *name,
			size_t maximum, struct json_object **array,
			size_t *count)
{
	if (!member(object, name, json_type_array, array))
		return false;
	*count = json_object_array_length(*array);
	return *count <= maximum;
}

static bool read_timer(struct json_object *object, struct clock_timer *timer)
{
	uint64_t state, generation, duration;
	if (!shape(object, 5) ||
	    !unsigned_value(object, "state", CLOCK_TIMER_FINISHED, &state) ||
	    !unsigned_value(object, "generation", UINT32_MAX, &generation) ||
	    !generation ||
	    !unsigned_value(object, "duration_seconds", CLOCK_TIMER_MAX_SECONDS,
			    &duration) ||
	    !unsigned_value(object, "remaining_ms",
			    CLOCK_TIMER_MAX_SECONDS * UINT64_C(1000),
			    &timer->remaining_ms) ||
	    !unsigned_value(object, "deadline_ms", UINT64_MAX,
			    &timer->deadline_ms))
		return false;
	timer->state = (enum clock_timer_state)state;
	timer->generation = (uint32_t)generation;
	timer->duration_seconds = (unsigned int)duration;
	return true;
}

static struct json_object *make_timer(const struct clock_timer *timer)
{
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	add_uint(object, "state", timer->state);
	add_uint(object, "generation", timer->generation);
	add_uint(object, "duration_seconds", timer->duration_seconds);
	add_uint(object, "remaining_ms", timer->remaining_ms);
	add_uint(object, "deadline_ms", timer->deadline_ms);
	return object;
}

static bool read_stopwatch(struct json_object *object,
			   struct clock_stopwatch *stopwatch)
{
	struct json_object *laps;
	if (!shape(object, 3) ||
	    !boolean_value(object, "running", &stopwatch->running) ||
	    !unsigned_value(object, "elapsed_ms", UINT64_MAX,
			    &stopwatch->elapsed_ms) ||
	    !array_value(object, "laps", CLOCK_LAP_LIMIT, &laps,
			 &stopwatch->lap_count))
		return false;
	for (size_t index = 0; index < stopwatch->lap_count; ++index) {
		struct json_object *lap =
			json_object_array_get_idx(laps, index);
		if (!shape(lap, 2) ||
		    !unsigned_value(lap, "elapsed_ms", UINT64_MAX,
				    &stopwatch->laps[index].elapsed_ms) ||
		    !unsigned_value(lap, "difference_ms", UINT64_MAX,
				    &stopwatch->laps[index].difference_ms))
			return false;
	}
	return true;
}

static struct json_object *
make_stopwatch(const struct clock_stopwatch *stopwatch)
{
	struct json_object *object = json_object_new_object();
	struct json_object *laps = json_object_new_array();
	if (!object || !laps) {
		if (object)
			json_object_put(object);
		if (laps)
			json_object_put(laps);
		return NULL;
	}
	add_bool(object, "running", stopwatch->running);
	add_uint(object, "elapsed_ms", stopwatch->elapsed_ms);
	for (size_t index = 0; index < stopwatch->lap_count; ++index) {
		struct json_object *lap = json_object_new_object();
		if (!lap) {
			json_object_put(object);
			json_object_put(laps);
			return NULL;
		}
		add_uint(lap, "elapsed_ms", stopwatch->laps[index].elapsed_ms);
		add_uint(lap, "difference_ms",
			 stopwatch->laps[index].difference_ms);
		json_object_array_add(laps, lap);
	}
	json_object_object_add(object, "laps", laps);
	return object;
}

static bool read_world(struct json_object *object,
		       struct clock_world_entry *entry)
{
	uint64_t id;
	if (!shape(object, 2) ||
	    !unsigned_value(object, "id", UINT32_MAX, &id) || !id ||
	    !offset_value(object, &entry->offset_minutes))
		return false;
	entry->id = (uint32_t)id;
	return true;
}

static struct json_object *make_world(const struct clock_world_entry *entry)
{
	struct json_object *object = json_object_new_object();
	if (!object)
		return NULL;
	add_uint(object, "id", entry->id);
	add_int(object, "offset_minutes", entry->offset_minutes);
	return object;
}

static bool read_model(struct json_object *object, struct clock_model *model)
{
	struct json_object *alarms, *active, *queue, *missed, *timer,
		*stopwatch, *world;
	uint64_t view, snooze, next_alarm, next_world, last_hour, last_minute;
	if (!shape(object, 17) ||
	    !wall_value(object, "wall_utc", CLOCK_WALL_MAX, false,
			&model->wall_utc) ||
	    !unsigned_value(object, "monotonic_ms", UINT64_MAX,
			    &model->monotonic_ms) ||
	    !unsigned_value(object, "view", CLOCK_VIEW_ANALOG, &view) ||
	    !unsigned_value(object, "snooze_minutes", 30, &snooze) || !snooze ||
	    !unsigned_value(object, "last_alarm_hour", 23, &last_hour) ||
	    !unsigned_value(object, "last_alarm_minute", 59, &last_minute) ||
	    !array_value(object, "alarms", CLOCK_ALARM_LIMIT, &alarms,
			 &model->alarm_count) ||
	    !member(object, "active", json_type_object, &active) ||
	    !read_event(active, &model->active) ||
	    model->active.missed_reason != CLOCK_MISSED_NONE ||
	    !array_value(object, "queue", CLOCK_QUEUE_LIMIT, &queue,
			 &model->queue_count) ||
	    !array_value(object, "missed", CLOCK_MISSED_LIMIT, &missed,
			 &model->missed_count) ||
	    !unsigned_value(object, "missed_total", UINT64_MAX,
			    &model->missed_total) ||
	    model->missed_total < model->missed_count ||
	    !member(object, "timer", json_type_object, &timer) ||
	    !read_timer(timer, &model->timer) ||
	    !member(object, "stopwatch", json_type_object, &stopwatch) ||
	    !read_stopwatch(stopwatch, &model->stopwatch) ||
	    !array_value(object, "world", CLOCK_WORLD_LIMIT, &world,
			 &model->world_count) ||
	    !unsigned_value(object, "next_alarm_id", UINT32_MAX, &next_alarm) ||
	    !unsigned_value(object, "next_world_id", UINT32_MAX, &next_world) ||
	    !unsigned_value(object, "next_event_id", UINT64_MAX,
			    &model->next_event_id))
		return false;
	model->view = (enum clock_view)view;
	model->snooze_minutes = (unsigned int)snooze;
	model->last_alarm_hour = (unsigned int)last_hour;
	model->last_alarm_minute = (unsigned int)last_minute;
	model->next_alarm_id = (uint32_t)next_alarm;
	model->next_world_id = (uint32_t)next_world;
	for (size_t index = 0; index < model->alarm_count; ++index) {
		if (!read_alarm(json_object_array_get_idx(alarms, index),
				&model->alarms[index]))
			return false;
		for (size_t previous = 0; previous < index; ++previous)
			if (model->alarms[index].id ==
			    model->alarms[previous].id)
				return false;
	}
	for (size_t index = 0; index < model->queue_count; ++index)
		if (!read_event(json_object_array_get_idx(queue, index),
				&model->queue[index]) ||
		    model->queue[index].kind == CLOCK_EVENT_NONE ||
		    model->queue[index].missed_reason != CLOCK_MISSED_NONE)
			return false;
	for (size_t index = 0; index < model->missed_count; ++index)
		if (!read_event(json_object_array_get_idx(missed, index),
				&model->missed[index]) ||
		    model->missed[index].kind == CLOCK_EVENT_NONE ||
		    model->missed[index].missed_reason == CLOCK_MISSED_NONE)
			return false;
	for (size_t index = 0; index < model->world_count; ++index) {
		if (!read_world(json_object_array_get_idx(world, index),
				&model->world[index]))
			return false;
		for (size_t previous = 0; previous < index; ++previous)
			if (model->world[index].id ==
				    model->world[previous].id ||
			    model->world[index].offset_minutes ==
				    model->world[previous].offset_minutes)
				return false;
	}
	return true;
}

static struct json_object *make_model(const struct clock_model *model)
{
	struct json_object *object, *alarms, *queue, *missed, *world;
	if (model->alarm_count > CLOCK_ALARM_LIMIT ||
	    model->queue_count > CLOCK_QUEUE_LIMIT ||
	    model->missed_count > CLOCK_MISSED_LIMIT ||
	    model->world_count > CLOCK_WORLD_LIMIT ||
	    model->stopwatch.lap_count > CLOCK_LAP_LIMIT)
		return NULL;
	object = json_object_new_object();
	alarms = json_object_new_array();
	queue = json_object_new_array();
	missed = json_object_new_array();
	world = json_object_new_array();
	if (!object || !alarms || !queue || !missed || !world) {
		if (object)
			json_object_put(object);
		if (alarms)
			json_object_put(alarms);
		if (queue)
			json_object_put(queue);
		if (missed)
			json_object_put(missed);
		if (world)
			json_object_put(world);
		return NULL;
	}
	add_int(object, "wall_utc", model->wall_utc);
	add_uint(object, "monotonic_ms", model->monotonic_ms);
	add_uint(object, "view", model->view);
	add_uint(object, "snooze_minutes", model->snooze_minutes);
	add_uint(object, "last_alarm_hour", model->last_alarm_hour);
	add_uint(object, "last_alarm_minute", model->last_alarm_minute);
	for (size_t index = 0; index < model->alarm_count; ++index)
		json_object_array_add(alarms,
				      make_alarm(&model->alarms[index]));
	json_object_object_add(object, "alarms", alarms);
	json_object_object_add(object, "active", make_event(&model->active));
	for (size_t index = 0; index < model->queue_count; ++index)
		json_object_array_add(queue, make_event(&model->queue[index]));
	json_object_object_add(object, "queue", queue);
	for (size_t index = 0; index < model->missed_count; ++index)
		json_object_array_add(missed,
				      make_event(&model->missed[index]));
	json_object_object_add(object, "missed", missed);
	add_uint(object, "missed_total", model->missed_total);
	json_object_object_add(object, "timer", make_timer(&model->timer));
	json_object_object_add(object, "stopwatch",
			       make_stopwatch(&model->stopwatch));
	for (size_t index = 0; index < model->world_count; ++index)
		json_object_array_add(world, make_world(&model->world[index]));
	json_object_object_add(object, "world", world);
	add_uint(object, "next_alarm_id", model->next_alarm_id);
	add_uint(object, "next_world_id", model->next_world_id);
	add_uint(object, "next_event_id", model->next_event_id);
	return object;
}

static bool read_preferences(struct json_object *object,
			     struct clock_model *model)
{
	struct json_object *alarms, *world;
	uint64_t view, snooze, next_alarm, next_world, duration;
	uint64_t last_hour, last_minute;

	if (!shape(object, 9) ||
	    !unsigned_value(object, "view", CLOCK_VIEW_ANALOG, &view) ||
	    !unsigned_value(object, "snooze_minutes", 30, &snooze) || !snooze ||
	    !unsigned_value(object, "last_alarm_hour", 23, &last_hour) ||
	    !unsigned_value(object, "last_alarm_minute", 59, &last_minute) ||
	    !unsigned_value(object, "timer_duration_seconds",
			    CLOCK_TIMER_MAX_SECONDS, &duration) ||
	    !unsigned_value(object, "next_alarm_id", UINT32_MAX, &next_alarm) ||
	    !unsigned_value(object, "next_world_id", UINT32_MAX, &next_world) ||
	    !array_value(object, "alarms", CLOCK_ALARM_LIMIT, &alarms,
			 &model->alarm_count) ||
	    !array_value(object, "world", CLOCK_WORLD_LIMIT, &world,
			 &model->world_count))
		return false;
	model->view = (enum clock_view)view;
	model->snooze_minutes = (unsigned int)snooze;
	model->last_alarm_hour = (unsigned int)last_hour;
	model->last_alarm_minute = (unsigned int)last_minute;
	model->timer.duration_seconds = (unsigned int)duration;
	model->timer.remaining_ms = duration * UINT64_C(1000);
	model->next_alarm_id = (uint32_t)next_alarm;
	model->next_world_id = (uint32_t)next_world;
	for (size_t index = 0; index < model->alarm_count; ++index) {
		struct json_object *value =
			json_object_array_get_idx(alarms, index);
		struct json_object *spec;
		struct clock_alarm *alarm = &model->alarms[index];
		uint64_t id, generation;

		if (!shape(value, 4) ||
		    !unsigned_value(value, "id", UINT32_MAX, &id) || !id ||
		    (next_alarm && id >= next_alarm) ||
		    !unsigned_value(value, "generation", UINT32_MAX,
				    &generation) ||
		    !generation ||
		    !member(value, "spec", json_type_object, &spec) ||
		    !read_alarm_spec(spec, &alarm->spec) ||
		    !wall_value(value, "last_due_utc", CLOCK_WALL_MAX, true,
				&alarm->last_due_utc) ||
		    alarm->last_due_utc % 60 != 0)
			return false;
		alarm->id = (uint32_t)id;
		alarm->generation = (uint32_t)generation;
		for (size_t previous = 0; previous < index; ++previous)
			if (alarm->id == model->alarms[previous].id)
				return false;
		if (alarm->spec.enabled) {
			int64_t after = model->wall_utc > alarm->last_due_utc ?
						model->wall_utc :
						alarm->last_due_utc;

			if (clock_model_alarm_next_due(&alarm->spec, after,
						       &alarm->next_due_utc) !=
			    CLOCK_OK)
				alarm->spec.enabled = false;
		}
	}
	for (size_t index = 0; index < model->world_count; ++index) {
		struct clock_world_entry *entry = &model->world[index];

		if (!read_world(json_object_array_get_idx(world, index),
				entry) ||
		    (next_world && entry->id >= next_world))
			return false;
		for (size_t previous = 0; previous < index; ++previous)
			if (entry->id == model->world[previous].id ||
			    entry->offset_minutes ==
				    model->world[previous].offset_minutes)
				return false;
	}
	return true;
}

static struct json_object *make_preferences(const struct clock_model *model)
{
	struct json_object *object, *alarms, *world;

	if (model->alarm_count > CLOCK_ALARM_LIMIT ||
	    model->world_count > CLOCK_WORLD_LIMIT)
		return NULL;
	object = json_object_new_object();
	alarms = json_object_new_array();
	world = json_object_new_array();
	if (!object || !alarms || !world) {
		if (object)
			json_object_put(object);
		if (alarms)
			json_object_put(alarms);
		if (world)
			json_object_put(world);
		return NULL;
	}
	add_uint(object, "view", model->view);
	add_uint(object, "snooze_minutes", model->snooze_minutes);
	add_uint(object, "last_alarm_hour", model->last_alarm_hour);
	add_uint(object, "last_alarm_minute", model->last_alarm_minute);
	add_uint(object, "timer_duration_seconds",
		 model->timer.duration_seconds);
	add_uint(object, "next_alarm_id", model->next_alarm_id);
	add_uint(object, "next_world_id", model->next_world_id);
	for (size_t index = 0; index < model->alarm_count; ++index) {
		const struct clock_alarm *alarm = &model->alarms[index];
		struct json_object *value = json_object_new_object();

		if (!value) {
			json_object_put(object);
			json_object_put(alarms);
			json_object_put(world);
			return NULL;
		}
		add_uint(value, "id", alarm->id);
		add_uint(value, "generation", alarm->generation);
		json_object_object_add(value, "spec",
				       make_alarm_spec(&alarm->spec));
		add_int(value, "last_due_utc", alarm->last_due_utc);
		json_object_array_add(alarms, value);
	}
	json_object_object_add(object, "alarms", alarms);
	for (size_t index = 0; index < model->world_count; ++index)
		json_object_array_add(world, make_world(&model->world[index]));
	json_object_object_add(object, "world", world);
	return object;
}

enum clock_status clock_wire_preferences_encode(const struct clock_model *model,
						char *json, size_t capacity)
{
	struct clock_model checked;
	struct json_object *object;

	if (!model || clock_model_init(&checked, model->wall_utc,
				       model->monotonic_ms) != CLOCK_OK)
		return CLOCK_INVALID;
	object = make_preferences(model);
	enum clock_status status =
		read_preferences(object, &checked) ?
			write_object(object, json, capacity) :
			CLOCK_INVALID;
	if (object)
		json_object_put(object);
	return status;
}

enum clock_status clock_wire_preferences_decode(const char *json, size_t length,
						struct clock_model *model)
{
	struct clock_model decoded;
	struct json_object *object;

	if (!model || clock_model_init(&decoded, model->wall_utc,
				       model->monotonic_ms) != CLOCK_OK)
		return CLOCK_INVALID;
	object = parse_object(json, length);
	bool valid = read_preferences(object, &decoded);
	if (object)
		json_object_put(object);
	if (!valid)
		return CLOCK_INVALID;
	*model = decoded;
	return CLOCK_OK;
}

static bool read_status(struct json_object *object, enum clock_status *status)
{
	for (unsigned int value = CLOCK_OK; value <= CLOCK_STORAGE_ERROR;
	     ++value)
		if (literal_value(object, "status",
				  clock_model_status_name(
					  (enum clock_status)value))) {
			*status = (enum clock_status)value;
			return true;
		}
	return false;
}

static const char *storage_name(enum fplinux_storage_state state)
{
	switch (state) {
	case FPLINUX_STORAGE_UNPUBLISHED:
		return "unpublished";
	case FPLINUX_STORAGE_PUBLISHED_UNSYNCED:
		return "published-unsynced";
	case FPLINUX_STORAGE_DURABLE:
		return "durable";
	}
	return NULL;
}

static bool read_result(struct json_object *object, struct clock_result *result)
{
	uint64_t error;
	bool has_storage = false;

	if (!read_status(object, &result->status) ||
	    !boolean_value(object, "applied", &result->applied) ||
	    !unsigned_value(object, "error", INT_MAX, &error))
		return false;
	for (unsigned int state = FPLINUX_STORAGE_UNPUBLISHED;
	     state <= FPLINUX_STORAGE_DURABLE; ++state)
		if (literal_value(object, "storage", storage_name(state))) {
			result->storage.state = state;
			has_storage = true;
			break;
		}
	result->storage.error = (int)error;
	return has_storage &&
	       (result->storage.state == FPLINUX_STORAGE_DURABLE ?
			!error :
			error != 0) &&
	       (result->status == CLOCK_OK ?
			result->applied :
			result->status == CLOCK_STORAGE_ERROR ||
				!result->applied);
}

static bool read_snapshot(struct json_object *object, struct clock_model *model,
			  struct clock_result *result, uint64_t *serial)
{
	struct json_object *data;
	return shape(object, 7) && literal_value(object, "op", "snapshot") &&
	       read_result(object, result) &&
	       unsigned_value(object, "serial", UINT64_MAX, serial) &&
	       member(object, "model", json_type_object, &data) &&
	       read_model(data, model);
}

enum clock_status clock_wire_snapshot_encode(const struct clock_model *model,
					     const struct clock_result *result,
					     uint64_t serial, char *json,
					     size_t capacity)
{
	struct json_object *object;
	struct clock_model checked = { 0 };
	struct clock_result checked_result;
	uint64_t checked_serial;
	if (!model || !result ||
	    (unsigned int)result->status > CLOCK_STORAGE_ERROR ||
	    !storage_name(result->storage.state))
		return CLOCK_INVALID;
	object = json_object_new_object();
	if (!object)
		return CLOCK_INVALID;
	json_object_object_add(object, "op",
			       json_object_new_string("snapshot"));
	json_object_object_add(object, "status",
			       json_object_new_string(clock_model_status_name(
				       result->status)));
	add_bool(object, "applied", result->applied);
	json_object_object_add(
		object, "storage",
		json_object_new_string(storage_name(result->storage.state)));
	add_int(object, "error", result->storage.error);
	add_uint(object, "serial", serial);
	json_object_object_add(object, "model", make_model(model));
	enum clock_status status =
		read_snapshot(object, &checked, &checked_result,
			      &checked_serial) ?
			write_object(object, json, capacity) :
			CLOCK_INVALID;
	json_object_put(object);
	return status;
}

enum clock_status clock_wire_snapshot_decode(const char *json, size_t length,
					     struct clock_model *model,
					     struct clock_result *result,
					     uint64_t *serial)
{
	struct clock_model decoded = { 0 };
	struct clock_result decoded_result;
	uint64_t decoded_serial;
	struct json_object *object;
	if (!model || !result || !serial)
		return CLOCK_INVALID;
	object = parse_object(json, length);
	bool valid = read_snapshot(object, &decoded, &decoded_result,
				   &decoded_serial);
	if (object)
		json_object_put(object);
	if (!valid)
		return CLOCK_INVALID;
	*model = decoded;
	*result = decoded_result;
	*serial = decoded_serial;
	return CLOCK_OK;
}

void clock_wire_ring_from_model(const struct clock_model *model,
				struct clock_ring *ring)
{
	if (!model || !ring)
		return;
	memset(ring, 0, sizeof(*ring));
	ring->event = model->active;
	ring->snooze_minutes = model->snooze_minutes;
	ring->missed_total = model->missed_total;
}

static bool read_ring(struct json_object *object, struct clock_ring *ring,
		      uint64_t *serial)
{
	struct json_object *event;
	uint64_t snooze;
	if (!shape(object, 5) || !literal_value(object, "op", "ring") ||
	    !unsigned_value(object, "serial", UINT64_MAX, serial) ||
	    !member(object, "event", json_type_object, &event) ||
	    !read_event(event, &ring->event) ||
	    ring->event.missed_reason != CLOCK_MISSED_NONE ||
	    !unsigned_value(object, "snooze_minutes", 30, &snooze) || !snooze ||
	    !unsigned_value(object, "missed_total", UINT64_MAX,
			    &ring->missed_total))
		return false;
	ring->snooze_minutes = (unsigned int)snooze;
	return true;
}

enum clock_status clock_wire_ring_encode(const struct clock_ring *ring,
					 uint64_t serial, char *json,
					 size_t capacity)
{
	struct clock_ring checked = { 0 };
	uint64_t checked_serial;
	struct json_object *object;
	if (!ring)
		return CLOCK_INVALID;
	object = json_object_new_object();
	if (!object)
		return CLOCK_INVALID;
	json_object_object_add(object, "op", json_object_new_string("ring"));
	add_uint(object, "serial", serial);
	json_object_object_add(object, "event", make_event(&ring->event));
	add_uint(object, "snooze_minutes", ring->snooze_minutes);
	add_uint(object, "missed_total", ring->missed_total);
	enum clock_status result =
		read_ring(object, &checked, &checked_serial) ?
			write_object(object, json, capacity) :
			CLOCK_INVALID;
	json_object_put(object);
	return result;
}

enum clock_status clock_wire_ring_decode(const char *json, size_t length,
					 struct clock_ring *ring,
					 uint64_t *serial)
{
	struct clock_ring decoded = { 0 };
	uint64_t decoded_serial;
	struct json_object *object;
	if (!ring || !serial)
		return CLOCK_INVALID;
	object = parse_object(json, length);
	bool valid = read_ring(object, &decoded, &decoded_serial);
	if (object)
		json_object_put(object);
	if (!valid)
		return CLOCK_INVALID;
	*ring = decoded;
	*serial = decoded_serial;
	return CLOCK_OK;
}
