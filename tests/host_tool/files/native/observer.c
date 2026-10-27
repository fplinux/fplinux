/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
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
#include <sys/stat.h>
#include <unistd.h>

static struct fplinux_ui *application_ui;
static struct files_observation observed;

bool __real_fplinux_language_load(const char *, const char *,
				  struct fplinux_language *);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_NATIVE_LANGUAGE_DIR");

	if (languages && directory &&
	    !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY))
		directory = languages;
	return __real_fplinux_language_load(directory, code, language);
}

struct fplinux_ui *__real_fplinux_ui_open(const char *, fplinux_ui_event_cb,
					  void *);

struct fplinux_ui *__wrap_fplinux_ui_open(const char *id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_ui = __real_fplinux_ui_open(id, callback, context);
	return application_ui;
}

char *__real_libintl_bindtextdomain(const char *, const char *);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_FILES_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

bool __real_fplinux_ui_menu_open(struct fplinux_ui *,
				 enum fplinux_ui_menu_origin, const char *,
				 const struct fplinux_ui_menu_item *, size_t,
				 fplinux_ui_menu_cb, void *);

bool __wrap_fplinux_ui_menu_open(struct fplinux_ui *ui,
				 enum fplinux_ui_menu_origin origin,
				 const char *title,
				 const struct fplinux_ui_menu_item *items,
				 size_t count, fplinux_ui_menu_cb callback,
				 void *context)
{
	assert(count <= 32);
	observed.menu_count = count;
	for (size_t i = 0; i < count; i++) {
		assert(strlen(items[i].text) < sizeof(observed.menu_items[i]));
		strcpy(observed.menu_items[i], items[i].text);
		observed.menu_separators[i] = items[i].separator_before;
	}
	return __real_fplinux_ui_menu_open(ui, origin, title, items, count,
					   callback, context);
}

/* Latency at the file-write boundary makes progress/lifecycle deterministic. */
ssize_t __real_write(int descriptor, const void *data, size_t size);

ssize_t __wrap_write(int descriptor, const void *data, size_t size)
{
	struct stat metadata;

	if (size >= 4096 && fstat(descriptor, &metadata) == 0 &&
	    S_ISREG(metadata.st_mode))
		usleep(20000);
	return __real_write(descriptor, data, size);
}

static void collect(lv_obj_t *object, size_t *used, bool menu)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_textarea_class))
		observed.cursor = lv_textarea_get_cursor_pos(object);
	if (lv_obj_check_type(object, &lv_image_class) &&
	    lv_image_get_src(object)) {
		observed.glyphs++;
		if (menu)
			observed.menu_glyphs++;
	}
	if (lv_obj_check_type(object, &lv_label_class)) {
		const char *text = lv_label_get_text(object);
		size_t length = strlen(text);

		assert(*used + length + 2 < sizeof(observed.labels));
		memcpy(observed.labels + *used, text, length);
		*used += length;
		observed.labels[(*used)++] = '\n';
		observed.labels[*used] = '\0';
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     index++)
		collect(lv_obj_get_child(object, (int32_t)index), used, menu);
}

void __real_lv_display_flush_ready(lv_display_t *display);

void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *descriptor = getenv("FPLINUX_FILES_OBSERVER_FD");
	size_t used = 1;

	if (descriptor && application_ui && lv_display_flush_is_last(display)) {
		struct fplinux_ui_screen *screen =
			fplinux_ui_current(application_ui);
		const char *text = fplinux_ui_text_box_get(screen);

		observed.frame++;
		observed.menu = fplinux_ui_menu_is_open(application_ui);
		observed.focused = fplinux_ui_list_get_focused(screen);
		observed.first_visible =
			fplinux_ui_list_get_first_visible(screen);
		observed.count = fplinux_ui_list_size(screen);
		observed.text[0] = '\0';
		observed.editor = text != NULL;
		observed.cursor = 0;
		observed.glyphs = 0;
		observed.menu_glyphs = 0;
		if (text) {
			assert(strlen(text) < sizeof(observed.text));
			strcpy(observed.text, text);
		}
		strcpy(observed.labels, "\n");
		collect(lv_display_get_screen_active(display), &used, false);
		collect(lv_display_get_layer_top(display), &used,
			observed.menu);
		collect(lv_display_get_layer_sys(display), &used, false);
		assert(send(atoi(descriptor), &observed, sizeof(observed),
			    MSG_NOSIGNAL) == (ssize_t)sizeof(observed));
	}
	__real_lv_display_flush_ready(display);
}
