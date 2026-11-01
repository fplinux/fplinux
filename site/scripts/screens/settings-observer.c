/* SPDX-License-Identifier: GPL-2.0-only */
#include "settings-observer.h"
#include "fplinux-language.h"

#include <assert.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>

static struct fplinux_ui *application_ui;
static unsigned int completed_frames;

struct fplinux_ui *__real_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context);

struct fplinux_ui *__wrap_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_ui = __real_fplinux_ui_open(app_id, callback, context);
	return application_ui;
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_SETTINGS_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

bool __real_fplinux_settings_read_os_name(const char *path, char *value,
					  size_t size);

bool __wrap_fplinux_settings_read_os_name(const char *path, char *value,
					  size_t size)
{
	const char *name = getenv("FPLINUX_SETTINGS_TEST_SYSTEM_NAME");

	if (!name)
		return __real_fplinux_settings_read_os_name(path, value, size);
	return snprintf(value, size, "%s", name) < (int)size;
}

bool __real_fplinux_settings_read_os_field(const char *path, const char *field,
					   char *value, size_t size);
bool __real_fplinux_settings_read_line(const char *path, char *value,
				       size_t size);
bool __real_fplinux_settings_read_memory(const char *path,
					 unsigned long *available,
					 unsigned long *total);
bool __real_fplinux_settings_about_logo(int profile, bool dark, char *path,
					size_t size);
int __real_uname(struct utsname *system);

bool __wrap_fplinux_settings_read_os_field(const char *path, const char *field,
					   char *value, size_t size)
{
	if (getenv("FPLINUX_SETTINGS_TEST_ABOUT") && !strcmp(field, "BUILD_ID"))
		return snprintf(value, size, "97b175e5e2e9") < (int)size;
	return __real_fplinux_settings_read_os_field(path, field, value, size);
}

bool __wrap_fplinux_settings_read_line(const char *path, char *value,
				       size_t size)
{
	if (getenv("FPLINUX_SETTINGS_TEST_ABOUT")) {
		if (!strcmp(path, "/proc/device-tree/model"))
			return snprintf(value, size, "UNIWA V1000 4G") <
			       (int)size;
		if (!strcmp(path, "/etc/alpine-release"))
			return snprintf(value, size, "3.24.2") < (int)size;
	}
	return __real_fplinux_settings_read_line(path, value, size);
}

bool __wrap_fplinux_settings_read_memory(const char *path,
					 unsigned long *available,
					 unsigned long *total)
{
	if (getenv("FPLINUX_SETTINGS_TEST_ABOUT")) {
		*available = 32768;
		*total = 65536;
		return true;
	}
	return __real_fplinux_settings_read_memory(path, available, total);
}

bool __wrap_fplinux_settings_about_logo(int profile, bool dark, char *path,
					size_t size)
{
	const char *brand = getenv("FPLINUX_SETTINGS_TEST_BRAND");
	char installed[128];
	const char *filename;

	if (!brand)
		return __real_fplinux_settings_about_logo(profile, dark, path,
							  size);
	if (!__real_fplinux_settings_about_logo(profile, dark, installed,
						sizeof(installed)))
		return false;
	filename = strrchr(installed, '/');
	assert(filename);
	return snprintf(path, size, "%s%s", brand, filename) < (int)size;
}

int __wrap_uname(struct utsname *system)
{
	int result = __real_uname(system);

	if (!result && getenv("FPLINUX_SETTINGS_TEST_ABOUT"))
		strcpy(system->release, "7.2.9-fplinux-5ccf7599bc43d193");
	return result;
}

static const char *language_directory(const char *directory)
{
	const char *languages = getenv("FPLINUX_SETTINGS_LANGUAGE_DIR");

	return languages && directory &&
			       !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY) ?
		       languages :
		       directory;
}

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	return __real_fplinux_language_load(language_directory(directory), code,
					    language);
}

size_t __real_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity);

size_t __wrap_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity)
{
	return __real_fplinux_language_list(language_directory(directory),
					    codes, capacity);
}

static void observe_about(lv_obj_t *object,
			  struct settings_frame_observation *observation)
{
	if (lv_obj_check_type(object, &lv_label_class) &&
	    !strcmp(lv_label_get_text(object), "Free memory")) {
		lv_obj_t *box = lv_obj_get_parent(object);
		lv_obj_t *value = lv_obj_get_child(box, 1);
		lv_obj_t *view = lv_obj_get_parent(box);
		lv_area_t value_area, view_area;

		if (value && view) {
			lv_obj_get_coords(value, &value_area);
			lv_obj_get_content_coords(view, &view_area);
			observation->about_memory_found = true;
			observation->about_memory_bottom = value_area.y2;
			observation->about_view_bottom = view_area.y2;
			observation->about_line_height =
				lv_font_get_line_height(
					lv_obj_get_style_text_font(
						value, LV_PART_MAIN));
			observation->about_scroll_y = lv_obj_get_scroll_y(view);
			observation->about_scroll_bottom =
				lv_obj_get_scroll_bottom(view);
		}
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		observe_about(lv_obj_get_child(object, (int32_t)index),
			      observation);
}

static void collect_labels(lv_obj_t *object,
			   struct settings_frame_observation *observation,
			   size_t *used)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_label_class)) {
		const char *text = lv_label_get_text(object);
		size_t length = strlen(text);
		static const char *const roots[4][2] = {
			{ "Display", "Экран" },
			{ "Time & language", "Время и язык" },
			{ "Input", "Ввод" },
			{ "About", "О системе" },
		};

		for (size_t index = 0; index < 4; ++index)
			if (!strcmp(text, roots[index][0]) ||
			    !strcmp(text, roots[index][1]))
				screen_observe_list_value(
					object,
					&observation->root_values[index]);
		if (!strcmp(text, "Bluetooth") && !observation->bluetooth.found)
			screen_observe_list_value(object,
						  &observation->bluetooth);

		assert(*used + length + 2 < sizeof(observation->labels));
		memcpy(observation->labels + *used, text, length);
		*used += length;
		observation->labels[(*used)++] = '\n';
		observation->labels[*used] = '\0';
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		collect_labels(lv_obj_get_child(object, (int32_t)index),
			       observation, used);
}

void __real_lv_display_flush_ready(lv_display_t *display);

/* Report the actual widget tree only after LVGL has painted its last area. */
void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *descriptor = getenv("FPLINUX_SETTINGS_OBSERVER_FD");
	struct settings_frame_observation observation = { .labels = "\n" };
	size_t used = 1;

	if (descriptor && application_ui && lv_display_flush_is_last(display)) {
		observation.frame = ++completed_frames;
		observation.theme = fplinux_ui_theme(application_ui)->kind;
		struct fplinux_ui_screen *screen =
			fplinux_ui_current(application_ui);

		observation.date_value = fplinux_ui_form_get_date(screen, 0);
		observation.time_value = fplinux_ui_form_get_date(screen, 1);
		observation.focused_item = fplinux_ui_form_get_focus(screen);
		observation.editing = fplinux_ui_form_is_editing(screen);
		for (size_t index = 0; index < 16; ++index)
			if (fplinux_ui_list_is_selected(
				    fplinux_ui_current(application_ui), index))
				observation.selection_mask |= UINT32_C(1)
							      << index;
		assert(strlen(fplinux_ui_locale(application_ui)) <
		       sizeof(observation.locale));
		strcpy(observation.locale, fplinux_ui_locale(application_ui));
		observe_about(lv_display_get_screen_active(display),
			      &observation);
		collect_labels(lv_display_get_screen_active(display),
			       &observation, &used);
		collect_labels(lv_display_get_layer_top(display), &observation,
			       &used);
		collect_labels(lv_display_get_layer_sys(display), &observation,
			       &used);
		assert(send(atoi(descriptor), &observation, sizeof(observation),
			    MSG_NOSIGNAL) == (ssize_t)sizeof(observation));
	}
	__real_lv_display_flush_ready(display);
}
