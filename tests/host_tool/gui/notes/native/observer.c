/* SPDX-License-Identifier: 0BSD */
#include "observer.h"
#include "fplinux-language.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>
#include <assert.h>
#include <libintl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct fplinux_ui *application_ui;
static struct fplinux_ui_screen *editor;
static unsigned int frames;
static unsigned int editors;

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_NATIVE_LANGUAGE_DIR");

	if (languages && directory &&
	    !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY))
		directory = languages;
	return __real_fplinux_language_load(directory, code, language);
}

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

struct fplinux_ui_screen *__real_fplinux_ui_text_box_new(struct fplinux_ui *ui,
							 const char *title,
							 const char *text,
							 size_t max_chars);

struct fplinux_ui_screen *__wrap_fplinux_ui_text_box_new(struct fplinux_ui *ui,
							 const char *title,
							 const char *text,
							 size_t max_chars)
{
	editor = __real_fplinux_ui_text_box_new(ui, title, text, max_chars);
	++editors;
	return editor;
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_NOTES_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

static void collect(lv_obj_t *object, struct notes_observation *observation,
		    size_t *used)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_textarea_class))
		observation->cursor = lv_textarea_get_cursor_pos(object);
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
		collect(lv_obj_get_child(object, (int32_t)index), observation,
			used);
}

void __real_lv_display_flush_ready(lv_display_t *display);

void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *descriptor = getenv("FPLINUX_NOTES_OBSERVER_FD");
	struct notes_observation observation = { .labels = "\n" };
	size_t used = 1;

	if (descriptor && application_ui && editor &&
	    lv_display_flush_is_last(display)) {
		const char *text = fplinux_ui_text_box_get(editor);

		observation.frame = ++frames;
		observation.editors = editors;
		observation.editor = fplinux_ui_current(application_ui) ==
				     editor;
		observation.menu = fplinux_ui_menu_is_open(application_ui);
		assert(strlen(text) < sizeof(observation.text));
		strcpy(observation.text, text);
		collect(lv_display_get_screen_active(display), &observation,
			&used);
		collect(lv_display_get_layer_top(display), &observation, &used);
		collect(lv_display_get_layer_sys(display), &observation, &used);
		assert(send(atoi(descriptor), &observation, sizeof(observation),
			    MSG_NOSIGNAL) == (ssize_t)sizeof(observation));
	}
	__real_lv_display_flush_ready(display);
}
