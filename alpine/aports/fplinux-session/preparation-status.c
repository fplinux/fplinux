/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "preparation.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const stage_names[] = {
	[FPLINUX_PREPARATION_WAITING] = "waiting",
	[FPLINUX_PREPARATION_READING] = "reading",
	[FPLINUX_PREPARATION_EXTRACTING] = "extracting",
	[FPLINUX_PREPARATION_READY] = "ready",
	[FPLINUX_PREPARATION_ERROR] = "error",
	[FPLINUX_PREPARATION_CANCELLED] = "cancelled",
};

bool fplinux_preparation_parse_stage(const char *name,
				     enum fplinux_preparation_stage *stage)
{
	for (size_t index = 0;
	     index < sizeof(stage_names) / sizeof(stage_names[0]); ++index) {
		if (!strcmp(name, stage_names[index])) {
			*stage = (enum fplinux_preparation_stage)index;
			return true;
		}
	}
	return false;
}

bool fplinux_preparation_is_busy(enum fplinux_preparation_stage stage)
{
	return stage == FPLINUX_PREPARATION_WAITING ||
	       stage == FPLINUX_PREPARATION_EXTRACTING;
}

static bool read_count(struct json_object *object, const char *name,
		       uint64_t *value)
{
	struct json_object *field;
	const char *text;

	if (!json_object_object_get_ex(object, name, &field) ||
	    !json_object_is_type(field, json_type_int))
		return false;
	text = json_object_get_string(field);
	if (!text || text[0] == '-')
		return false;
	*value = json_object_get_uint64(field);
	return true;
}

static bool parse_status(const char *text, size_t size,
			 struct fplinux_preparation_status *status)
{
	struct fplinux_preparation_status parsed = { 0 };
	struct json_tokener *tokener = json_tokener_new();
	struct json_object *object = NULL;
	struct json_object *stage;
	size_t end;
	bool valid = false;

	if (!tokener)
		return false;
	json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
	object = json_tokener_parse_ex(tokener, text, (int)size);
	end = json_tokener_get_parse_end(tokener);
	if (json_tokener_get_error(tokener) != json_tokener_success ||
	    !json_object_is_type(object, json_type_object))
		goto done;
	while (end < size && isspace((unsigned char)text[end]))
		++end;
	if (end != size ||
	    !json_object_object_get_ex(object, "stage", &stage) ||
	    !json_object_is_type(stage, json_type_string) ||
	    strlen(json_object_get_string(stage)) !=
		    (size_t)json_object_get_string_len(stage) ||
	    !fplinux_preparation_parse_stage(json_object_get_string(stage),
					     &parsed.stage))
		goto done;
	if (parsed.stage == FPLINUX_PREPARATION_READING) {
		if (!read_count(object, "completed", &parsed.completed) ||
		    !read_count(object, "total", &parsed.total) ||
		    !parsed.total || parsed.completed > parsed.total)
			goto done;
	} else if (json_object_object_get_ex(object, "completed", &stage) ||
		   json_object_object_get_ex(object, "total", &stage)) {
		goto done;
	}
	*status = parsed;
	valid = true;
done:
	json_object_put(object);
	json_tokener_free(tokener);
	if (!valid)
		errno = EINVAL;
	return valid;
}

bool fplinux_preparation_read(const char *path,
			      struct fplinux_preparation_status *status)
{
	char text[512];
	size_t used = 0;
	ssize_t count;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int saved_errno;
	bool valid = false;

	if (fd < 0) {
		if (errno != ENOENT)
			return false;
		*status = (struct fplinux_preparation_status){
			.stage = FPLINUX_PREPARATION_WAITING,
		};
		return true;
	}
	while (used < sizeof(text)) {
		count = read(fd, text + used, sizeof(text) - used);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			goto done;
		}
		if (!count) {
			valid = parse_status(text, used, status);
			goto done;
		}
		used += (size_t)count;
	}
	errno = EFBIG;
done:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return valid;
}

bool fplinux_preparation_write(const char *path,
			       const struct fplinux_preparation_status *status)
{
	struct json_object *object = json_object_new_object();
	const char *text;
	char *temporary = NULL;
	size_t size;
	size_t written = 0;
	int fd = -1;
	int saved_errno;
	bool success = false;

	if (!object)
		return false;
	json_object_object_add(
		object, "stage",
		json_object_new_string(stage_names[status->stage]));
	if (status->stage == FPLINUX_PREPARATION_READING) {
		json_object_object_add(
			object, "completed",
			json_object_new_uint64(status->completed));
		json_object_object_add(object, "total",
				       json_object_new_uint64(status->total));
	}
	text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	if (!text)
		goto done;
	size = strlen(text);
	temporary = malloc(strlen(path) + sizeof(".XXXXXX"));
	if (!temporary)
		goto done;
	sprintf(temporary, "%s.XXXXXX", path);
	fd = mkostemp(temporary, O_CLOEXEC);
	if (fd < 0)
		goto done;
	while (written < size) {
		ssize_t count = write(fd, text + written, size - written);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EIO;
			goto done;
		}
		written += (size_t)count;
	}
	if (close(fd) < 0) {
		fd = -1;
		goto done;
	}
	fd = -1;
	if (rename(temporary, path) < 0)
		goto done;
	success = true;
done:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (temporary && !success)
		unlink(temporary);
	free(temporary);
	json_object_put(object);
	errno = saved_errno;
	return success;
}
