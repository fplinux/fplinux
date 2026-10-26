/* SPDX-License-Identifier: GPL-2.0-only */
#include "settings-observer.h"
#include "fplinux-language.h"

#include <assert.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

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

static void collect_labels(lv_obj_t *object,
			   struct settings_frame_observation *observation,
			   size_t *used)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_label_class)) {
		const char *text = lv_label_get_text(object);
		size_t length = strlen(text);

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
