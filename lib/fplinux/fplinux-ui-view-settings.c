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

static const char *setting_label(enum fplinux_ui_view_setting setting)
{
	if (setting == FPLINUX_UI_VIEW_SCALE)
		return dgettext(FPLINUX_UI_TEXT_DOMAIN, "Scale");
	return dgettext(FPLINUX_UI_TEXT_DOMAIN, "Rotation");
}

static const char *setting_value(enum fplinux_ui_view_setting setting,
				 size_t value)
{
	static const char *const rotations[] = { "0°", "90°", "270°" };

	if (setting == FPLINUX_UI_VIEW_ROTATION)
		return rotations[value];
	if (value == 0)
		return dgettext(FPLINUX_UI_TEXT_DOMAIN, "Fit");
	if (value == 1)
		return "1:1";
	return dgettext(FPLINUX_UI_TEXT_DOMAIN, "Integer");
}

struct fplinux_ui_screen *fplinux_ui_view_settings_list_new(
	struct fplinux_ui *ui, const char *title,
	const struct fplinux_ui_view_settings *settings)
{
	struct fplinux_ui_screen *list;
	const char *glyphs[] = { "fullscreen", "rotate" };
	size_t values[2];

	if (!valid(settings)) {
		errno = EINVAL;
		return NULL;
	}
	list = fplinux_ui_list_new(ui, title, FPLINUX_UI_LIST_IMPLICIT);
	if (!list)
		return NULL;
	values[0] = settings->scale;
	values[1] = settings->rotation;
	for (size_t i = 0; i < 2; ++i) {
		size_t row =
			fplinux_ui_list_append(list, setting_label(i), NULL);

		if (row == SIZE_MAX ||
		    !fplinux_ui_list_set_icon(
			    list, row,
			    fplinux_ui_glyph(ui, glyphs[i],
					     FPLINUX_UI_GLYPH_ROW),
			    FPLINUX_UI_LIST_ICON_IMAGE) ||
		    !fplinux_ui_list_set_trailing_value(
			    list, row, setting_value(i, values[i])))
			return NULL;
	}
	return list;
}

struct fplinux_ui_screen *fplinux_ui_view_settings_choice_new(
	struct fplinux_ui *ui, enum fplinux_ui_view_setting setting,
	const struct fplinux_ui_view_settings *settings)
{
	struct fplinux_ui_screen *list;
	size_t current;

	if (!valid(settings) ||
	    (unsigned int)setting > FPLINUX_UI_VIEW_ROTATION) {
		errno = EINVAL;
		return NULL;
	}
	list = fplinux_ui_list_new(ui, setting_label(setting),
				   FPLINUX_UI_LIST_EXCLUSIVE);
	if (!list)
		return NULL;
	for (size_t i = 0; i < 3; ++i)
		if (fplinux_ui_list_append(list, setting_value(setting, i),
					   NULL) == SIZE_MAX)
			return NULL;
	current = setting == FPLINUX_UI_VIEW_SCALE ? settings->scale :
						     settings->rotation;
	fplinux_ui_list_set_selected(list, current, true);
	fplinux_ui_list_set_focused(list, current);
	return list;
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
