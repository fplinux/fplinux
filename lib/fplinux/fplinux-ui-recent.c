/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-ui-recent.h"
#include "fplinux-ui-prefs.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#define RECENT_KEY "recent-documents"

void fplinux_ui_recent_cleanup(struct fplinux_ui_recent *recent)
{
	if (!recent)
		return;
	for (size_t i = 0; i < recent->count; ++i) {
		free(recent->items[i].path);
		free(recent->items[i].title);
	}
	memset(recent, 0, sizeof(*recent));
}

bool fplinux_ui_recent_remove(struct fplinux_ui_recent *recent, size_t index)
{
	if (!recent || index >= recent->count) {
		errno = EINVAL;
		return false;
	}
	free(recent->items[index].path);
	free(recent->items[index].title);
	--recent->count;
	memmove(&recent->items[index], &recent->items[index + 1],
		(recent->count - index) * sizeof(recent->items[0]));
	memset(&recent->items[recent->count], 0, sizeof(recent->items[0]));
	return true;
}

bool fplinux_ui_recent_record(struct fplinux_ui_recent *recent,
			      const char *path, const char *title)
{
	char *path_copy, *title_copy = NULL;

	if (!recent || !path || path[0] != '/') {
		errno = EINVAL;
		return false;
	}
	path_copy = strdup(path);
	if (!path_copy)
		return false;
	if (title && !(title_copy = strdup(title))) {
		free(path_copy);
		return false;
	}
	for (size_t i = 0; i < recent->count; ++i) {
		if (!strcmp(recent->items[i].path, path)) {
			fplinux_ui_recent_remove(recent, i);
			break;
		}
	}
	if (recent->count == FPLINUX_UI_RECENT_MAX)
		fplinux_ui_recent_remove(recent, recent->count - 1);
	for (size_t i = recent->count; i > 0; --i)
		recent->items[i] = recent->items[i - 1];
	recent->items[0] =
		(struct fplinux_ui_recent_entry){ path_copy, title_copy };
	++recent->count;
	return true;
}

static const char *string_field(struct json_object *object, const char *name)
{
	struct json_object *field;
	const char *value;

	if (!json_object_object_get_ex(object, name, &field) ||
	    !json_object_is_type(field, json_type_string))
		return NULL;
	value = json_object_get_string(field);
	if (strlen(value) != (size_t)json_object_get_string_len(field))
		return NULL;
	return value;
}

bool fplinux_ui_recent_load(struct fplinux_ui_recent *recent,
			    const struct fplinux_ui_prefs *prefs)
{
	struct fplinux_ui_recent loaded = { 0 };
	struct json_tokener *tokener;
	struct json_object *array;
	const char *text;
	size_t count;
	int error;

	if (!recent || !prefs) {
		errno = EINVAL;
		return false;
	}
	text = fplinux_ui_prefs_get(prefs, RECENT_KEY, NULL);
	if (!text) {
		fplinux_ui_recent_cleanup(recent);
		return true;
	}
	tokener = json_tokener_new();
	if (!tokener)
		return false;
	json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
	array = json_tokener_parse_ex(tokener, text, -1);
	if (json_tokener_get_error(tokener) != json_tokener_success || !array ||
	    !json_object_is_type(array, json_type_array)) {
		error = EINVAL;
		goto fail;
	}
	count = json_object_array_length(array);
	if (count > FPLINUX_UI_RECENT_MAX) {
		error = EINVAL;
		goto fail;
	}
	for (size_t i = count; i > 0; --i) {
		struct json_object *item =
			json_object_array_get_idx(array, i - 1);
		struct json_object *field;
		const char *path, *title = NULL;

		if (!json_object_is_type(item, json_type_object) ||
		    !(path = string_field(item, "path"))) {
			error = EINVAL;
			goto fail;
		}
		if (json_object_object_get_ex(item, "title", &field) &&
		    !(title = string_field(item, "title"))) {
			error = EINVAL;
			goto fail;
		}
		if (!fplinux_ui_recent_record(&loaded, path, title)) {
			error = errno;
			goto fail;
		}
	}
	json_object_put(array);
	json_tokener_free(tokener);
	fplinux_ui_recent_cleanup(recent);
	*recent = loaded;
	return true;
fail:
	json_object_put(array);
	json_tokener_free(tokener);
	fplinux_ui_recent_cleanup(&loaded);
	errno = error;
	return false;
}

bool fplinux_ui_recent_store(const struct fplinux_ui_recent *recent,
			     struct fplinux_ui_prefs *prefs)
{
	struct json_object *array;
	bool result = false;

	if (!recent || !prefs || recent->count > FPLINUX_UI_RECENT_MAX) {
		errno = EINVAL;
		return false;
	}
	array = json_object_new_array();
	if (!array)
		return false;
	for (size_t i = 0; i < recent->count; ++i) {
		const struct fplinux_ui_recent_entry *entry = &recent->items[i];
		struct json_object *item = json_object_new_object();
		struct json_object *path =
			entry->path ? json_object_new_string(entry->path) :
				      NULL;
		struct json_object *title =
			entry->title ? json_object_new_string(entry->title) :
				       NULL;

		if (!item || !path || (entry->title && !title)) {
			json_object_put(item);
			json_object_put(path);
			json_object_put(title);
			errno = ENOMEM;
			goto done;
		}
		if (json_object_object_add(item, "path", path) < 0) {
			json_object_put(path);
			json_object_put(title);
			json_object_put(item);
			errno = ENOMEM;
			goto done;
		}
		if (title && json_object_object_add(item, "title", title) < 0) {
			json_object_put(title);
			json_object_put(item);
			errno = ENOMEM;
			goto done;
		}
		if (json_object_array_add(array, item) < 0) {
			json_object_put(item);
			errno = ENOMEM;
			goto done;
		}
	}
	result = fplinux_ui_prefs_set(
		prefs, RECENT_KEY,
		json_object_to_json_string_ext(array, JSON_C_TO_STRING_PLAIN));
done:
	json_object_put(array);
	return result;
}
