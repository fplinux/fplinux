/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-view-settings.h"
#include "fplinux-ui-prefs.h"
#include "fplinux-ui-screen-model.h"
#include "fplinux-ui-screen.h"

#include <errno.h>
#include <json-c/json.h>
#include <libintl.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VIEW_KEY "view-settings"

static const char *const scale_values[] = { "fit", "one-to-one", "integer" };
static const char *const rotation_values[] = { "0", "90", "270" };

static bool valid(const struct fplinux_ui_view_settings *settings)
{
	return settings && (unsigned int)settings->scale < 3 &&
	       (unsigned int)settings->rotation < 3;
}

struct fplinux_ui_view_settings fplinux_ui_view_settings_defaults(void)
{
	return (struct fplinux_ui_view_settings){ FPLINUX_RGB565_FIT,
						  FPLINUX_RGB565_ROTATE_0 };
}

static bool read_field(struct json_object *object, const char *name,
		       const char *const *values, int *destination)
{
	struct json_object *field;
	const char *text;

	if (!json_object_object_get_ex(object, name, &field))
		return true;
	if (!json_object_is_type(field, json_type_string))
		return false;
	text = json_object_get_string(field);
	if (strlen(text) != (size_t)json_object_get_string_len(field))
		return false;
	for (int i = 0; i < 3; ++i) {
		if (!strcmp(text, values[i])) {
			*destination = i;
			return true;
		}
	}
	return false;
}

bool fplinux_ui_view_settings_load(
	struct fplinux_ui_view_settings *settings,
	const struct fplinux_ui_prefs *prefs,
	const struct fplinux_ui_view_settings *defaults)
{
	struct fplinux_ui_view_settings loaded;
	struct json_tokener *tokener;
	struct json_object *object;
	const char *text;
	int scale, rotation;
	bool ok;

	if (!settings || !prefs || !valid(defaults)) {
		errno = EINVAL;
		return false;
	}
	loaded = *defaults;
	text = fplinux_ui_prefs_get(prefs, VIEW_KEY, NULL);
	if (!text) {
		*settings = loaded;
		return true;
	}
	tokener = json_tokener_new();
	if (!tokener)
		return false;
	json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
	object = json_tokener_parse_ex(tokener, text, -1);
	scale = loaded.scale;
	rotation = loaded.rotation;
	ok = json_tokener_get_error(tokener) == json_tokener_success &&
	     object && json_object_is_type(object, json_type_object) &&
	     read_field(object, "scale", scale_values, &scale) &&
	     read_field(object, "rotation", rotation_values, &rotation);
	json_object_put(object);
	json_tokener_free(tokener);
	if (!ok) {
		errno = EINVAL;
		return false;
	}
	loaded.scale = scale;
	loaded.rotation = rotation;
	*settings = loaded;
	return true;
}

bool fplinux_ui_view_settings_store(
	const struct fplinux_ui_view_settings *settings,
	struct fplinux_ui_prefs *prefs)
{
	char value[80];

	if (!valid(settings) || !prefs) {
		errno = EINVAL;
		return false;
	}
	snprintf(value, sizeof(value), "{\"scale\":\"%s\",\"rotation\":\"%s\"}",
		 scale_values[settings->scale],
		 rotation_values[settings->rotation]);
	return fplinux_ui_prefs_set(prefs, VIEW_KEY, value);
}

bool fplinux_ui_view_settings_reset(struct fplinux_ui_prefs *prefs)
{
	return fplinux_ui_prefs_remove(prefs, VIEW_KEY);
}

bool fplinux_ui_view_settings_form_append(
	struct fplinux_ui_screen *form,
	const struct fplinux_ui_view_settings *settings,
	struct fplinux_ui_view_fields *fields)
{
	const char *scale[] = { dgettext(FPLINUX_UI_TEXT_DOMAIN, "Fit"),
				dgettext(FPLINUX_UI_TEXT_DOMAIN,
					 "Original size"),
				dgettext(FPLINUX_UI_TEXT_DOMAIN, "Integer") };
	const char *rotation[] = { "0°", "90°", "270°" };
	struct fplinux_ui_view_fields added;
	int error;

	if (!form || !valid(settings) || !fields) {
		errno = EINVAL;
		return false;
	}
	added.scale = fplinux_ui_form_append_choice(
		form, dgettext(FPLINUX_UI_TEXT_DOMAIN, "Scale"),
		FPLINUX_UI_CHOICE_POPUP, scale, 3);
	if (added.scale == SIZE_MAX)
		return false;
	added.rotation = fplinux_ui_form_append_choice(
		form, dgettext(FPLINUX_UI_TEXT_DOMAIN, "Rotation"),
		FPLINUX_UI_CHOICE_POPUP, rotation, 3);
	if (added.rotation == SIZE_MAX) {
		error = errno;
		fplinux_ui_form_delete(form, added.scale);
		errno = error;
		return false;
	}
	fplinux_ui_form_set_choice(form, added.scale, settings->scale);
	fplinux_ui_form_set_choice(form, added.rotation, settings->rotation);
	*fields = added;
	return true;
}

bool fplinux_ui_view_settings_form_read(
	const struct fplinux_ui_screen *form,
	const struct fplinux_ui_view_fields *fields,
	struct fplinux_ui_view_settings *settings)
{
	size_t scale, rotation;

	if (!form || !fields || !settings) {
		errno = EINVAL;
		return false;
	}
	scale = fplinux_ui_form_get_choice(form, fields->scale);
	rotation = fplinux_ui_form_get_choice(form, fields->rotation);
	if (scale >= 3 || rotation >= 3) {
		errno = EINVAL;
		return false;
	}
	settings->scale = scale;
	settings->rotation = rotation;
	return true;
}

struct fplinux_ui_prefs *fplinux_ui_program_prefs_open(const char *app_id,
						       const char *identity_key)
{
	char directory[PATH_MAX];
	size_t length;

	if (!identity_key || strlen(identity_key) != 64) {
		errno = EINVAL;
		return NULL;
	}
	for (size_t i = 0; i < 64; ++i) {
		char c = identity_key[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			errno = EINVAL;
			return NULL;
		}
	}
	if (!fplinux_ui_state_dir(app_id, directory, sizeof(directory)))
		return NULL;
	length = strlen(directory);
	if (snprintf(directory + length, sizeof(directory) - length,
		     "/programs/%s",
		     identity_key) >= (int)(sizeof(directory) - length)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	return fplinux_ui_prefs_open_directory(directory);
}
