/* SPDX-License-Identifier: GPL-2.0-only */
#include "apidemo-observer.h"
#include "fplinux-language.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct fplinux_ui_screen *preferences_form;
static size_t preference_items[3];
static const char *const preference_labels[] = {
	"Stored",
	"In memory",
	"Save result",
};

static void observe(enum apidemo_observation_kind kind, const char *title,
		    const char *text)
{
	const char *descriptor = getenv("FPLINUX_APIDEMO_OBSERVER_FD");
	struct apidemo_observation observation = { .kind = kind };

	if (!descriptor)
		return;
	snprintf(observation.title, sizeof(observation.title), "%s",
		 title ? title : "");
	snprintf(observation.text, sizeof(observation.text), "%s",
		 text ? text : "");
	assert(send(atoi(descriptor), &observation, sizeof(observation), 0) ==
	       (ssize_t)sizeof(observation));
}

void __real_lv_label_set_text(lv_obj_t *label, const char *text);

void __wrap_lv_label_set_text(lv_obj_t *label, const char *text)
{
	__real_lv_label_set_text(label, text);
	if (text && text[0])
		observe(APIDEMO_LABEL, NULL, lv_label_get_text(label));
}

void __wrap_lv_label_set_text_fmt(lv_obj_t *label, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	lv_label_set_text_vfmt(label, format, arguments);
	va_end(arguments);
	observe(APIDEMO_LABEL, NULL, lv_label_get_text(label));
}

bool __real_fplinux_ui_text_set(struct fplinux_ui_text *field,
				const char *text);

bool __wrap_fplinux_ui_text_set(struct fplinux_ui_text *field, const char *text)
{
	bool changed = __real_fplinux_ui_text_set(field, text);

	if (changed)
		observe(APIDEMO_LABEL, "Raw text", fplinux_ui_text_get(field));
	return changed;
}

static void textarea_changed(lv_event_t *event)
{
	lv_obj_t *textarea = lv_event_get_target_obj(event);

	observe(APIDEMO_TEXTAREA, NULL, lv_textarea_get_text(textarea));
}

lv_obj_t *__real_lv_textarea_create(lv_obj_t *parent);

lv_obj_t *__wrap_lv_textarea_create(lv_obj_t *parent)
{
	lv_obj_t *textarea = __real_lv_textarea_create(parent);

	if (textarea)
		assert(lv_obj_add_event_cb(textarea, textarea_changed,
					   LV_EVENT_VALUE_CHANGED, NULL));
	return textarea;
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_APIDEMO_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

/* Parse the actual packaged languages from the source checkout. */
bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_APIDEMO_LANGUAGE_DIR");

	if (languages && directory &&
	    !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY))
		directory = languages;
	return __real_fplinux_language_load(directory, code, language);
}

struct fplinux_ui_screen *
__real_fplinux_ui_list_new(struct fplinux_ui *ui, const char *title,
			   enum fplinux_ui_list_kind kind);

struct fplinux_ui_screen *
__wrap_fplinux_ui_list_new(struct fplinux_ui *ui, const char *title,
			   enum fplinux_ui_list_kind kind)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_list_new(ui, title, kind);

	if (screen)
		observe(APIDEMO_LIST, title, NULL);
	return screen;
}

struct fplinux_ui_screen *
__real_fplinux_ui_list_new_virtual(struct fplinux_ui *ui, const char *title,
				   enum fplinux_ui_list_kind kind, size_t count,
				   fplinux_ui_virtual_row_cb callback,
				   void *context);

struct fplinux_ui_screen *
__wrap_fplinux_ui_list_new_virtual(struct fplinux_ui *ui, const char *title,
				   enum fplinux_ui_list_kind kind, size_t count,
				   fplinux_ui_virtual_row_cb callback,
				   void *context)
{
	struct fplinux_ui_screen *screen = __real_fplinux_ui_list_new_virtual(
		ui, title, kind, count, callback, context);

	if (screen)
		observe(APIDEMO_LIST, title, NULL);
	return screen;
}

struct fplinux_ui_screen *__real_fplinux_ui_form_new(struct fplinux_ui *ui,
						     const char *title);

struct fplinux_ui_screen *__wrap_fplinux_ui_form_new(struct fplinux_ui *ui,
						     const char *title)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_form_new(ui, title);

	preferences_form = screen && title && !strcmp(title, "Preferences") ?
				   screen :
				   NULL;
	for (size_t index = 0; index < 3; ++index)
		preference_items[index] = SIZE_MAX;
	if (screen)
		observe(APIDEMO_FORM, title, NULL);
	return screen;
}

size_t __real_fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
					    const char *label,
					    const char *text);

size_t __wrap_fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
					    const char *label, const char *text)
{
	size_t item = __real_fplinux_ui_form_append_string(form, label, text);

	if (form == preferences_form && label)
		for (size_t index = 0; index < 3; ++index)
			if (!strcmp(label, preference_labels[index]))
				preference_items[index] = item;
	return item;
}

bool __real_fplinux_ui_form_set_string(struct fplinux_ui_screen *form,
				       size_t item, const char *text);

bool __wrap_fplinux_ui_form_set_string(struct fplinux_ui_screen *form,
				       size_t item, const char *text)
{
	bool changed = __real_fplinux_ui_form_set_string(form, item, text);

	if (changed && form == preferences_form)
		for (size_t index = 0; index < 3; ++index)
			if (preference_items[index] == item)
				observe(APIDEMO_PREFERENCE,
					preference_labels[index], text);
	return changed;
}

struct fplinux_storage_result
__real_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs);

struct fplinux_storage_result
__wrap_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	const char *path = getenv("FPLINUX_APIDEMO_SAVE_FAILURE");
	FILE *gate = path ? fopen(path, "r") : NULL;
	int mode = 0;

	if (gate) {
		mode = fgetc(gate);
		assert(!fclose(gate));
	}
	if (mode == 'f')
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED, .error = ENOSPC
		};
	if (mode == 'u') {
		struct fplinux_storage_result result =
			__real_fplinux_ui_prefs_save(prefs);

		assert(result.state == FPLINUX_STORAGE_DURABLE);
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
			.error = EIO
		};
	}
	return __real_fplinux_ui_prefs_save(prefs);
}

struct fplinux_ui_screen *
__real_fplinux_ui_alert_new(struct fplinux_ui *ui,
			    enum fplinux_ui_alert_kind kind, const char *title,
			    const char *text, unsigned int timeout_ms);

struct fplinux_ui_screen *
__wrap_fplinux_ui_alert_new(struct fplinux_ui *ui,
			    enum fplinux_ui_alert_kind kind, const char *title,
			    const char *text, unsigned int timeout_ms)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_alert_new(ui, kind, title, text, timeout_ms);

	if (screen)
		observe(APIDEMO_ALERT, title, text);
	return screen;
}

struct fplinux_ui_screen *__real_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title);

struct fplinux_ui_screen *__wrap_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_canvas_new(ui, title);

	if (screen)
		observe(APIDEMO_CANVAS, title, NULL);
	return screen;
}
