/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "apidemo-observer.h"
#include "fplinux-language.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session-internal.h>
#include <libintl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct fplinux_ui *application;
static lv_obj_t *canvas_area;
static struct fplinux_ui_screen *preferences_form;
static size_t preference_items[3];
static const char *const preference_labels[] = {
	"Stored",
	"In memory",
	"Save result",
};

int __real_open(const char *path, int flags, ...);

/* Preserve resource lookup and decoding; the capture owns the installed tree. */
int __wrap_open(const char *path, int flags, ...)
{
	static const char prefix[] = "/usr/share/fplinux/glyphs/";
	const char *root = getenv("FPLINUX_NATIVE_GLYPHS");
	char mapped[1024];
	mode_t mode = 0;
	va_list arguments;

	if (root && !strncmp(path, prefix, sizeof(prefix) - 1)) {
		if (snprintf(mapped, sizeof(mapped), "%s/%s", root,
			     path + sizeof(prefix) - 1) >=
		    (int)sizeof(mapped)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		path = mapped;
	}
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	return __real_open(path, flags, mode);
}

static struct apidemo_rectangle rectangle(lv_obj_t *object)
{
	lv_area_t bounds;

	lv_obj_get_coords(object, &bounds);
	return (struct apidemo_rectangle){
		.left = bounds.x1,
		.top = bounds.y1,
		.width = lv_area_get_width(&bounds),
		.height = lv_area_get_height(&bounds),
	};
}

static struct apidemo_rectangle content_rectangle(lv_obj_t *object)
{
	lv_area_t bounds;

	lv_obj_get_content_coords(object, &bounds);
	return (struct apidemo_rectangle){
		.left = bounds.x1,
		.top = bounds.y1,
		.width = lv_area_get_width(&bounds),
		.height = lv_area_get_height(&bounds),
	};
}

static void collect_canvas(lv_obj_t *object, int parent,
			   struct apidemo_frame_observation *observation)
{
	struct apidemo_canvas_object_observation *item;
	int index;

	if (lv_obj_is_hidden(object))
		return;
	assert(observation->canvas_object_count <
	       sizeof(observation->canvas_objects) /
		       sizeof(observation->canvas_objects[0]));
	index = (int)observation->canvas_object_count++;
	item = &observation->canvas_objects[index];
	item->parent = parent;
	item->bounds = rectangle(object);
	item->content = content_rectangle(object);
	item->border = lv_obj_get_style_border_width(object, LV_PART_MAIN);
	item->padding[0] = lv_obj_get_style_pad_left(object, LV_PART_MAIN);
	item->padding[1] = lv_obj_get_style_pad_top(object, LV_PART_MAIN);
	item->padding[2] = lv_obj_get_style_pad_right(object, LV_PART_MAIN);
	item->padding[3] = lv_obj_get_style_pad_bottom(object, LV_PART_MAIN);
	item->scroll_y = lv_obj_get_scroll_y(object);
	item->max_height = lv_obj_get_style_max_height(object, LV_PART_MAIN);
	if (lv_obj_check_type(object, &lv_label_class)) {
		item->line_height = lv_font_get_line_height(
			lv_obj_get_style_text_font(object, LV_PART_MAIN));
		snprintf(item->text, sizeof(item->text), "%s",
			 lv_label_get_text(object));
	}
	for (uint32_t child = 0; child < lv_obj_get_child_count(object);
	     ++child)
		collect_canvas(lv_obj_get_child(object, (int32_t)child), index,
			       observation);
}

static lv_obj_t *find_image(lv_obj_t *object)
{
	if (lv_obj_check_type(object, &lv_image_class))
		return object;
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index) {
		lv_obj_t *image =
			find_image(lv_obj_get_child(object, (int32_t)index));

		if (image)
			return image;
	}
	return NULL;
}

static void collect(lv_obj_t *object,
		    struct apidemo_frame_observation *observation, bool menu)
{
	if (lv_obj_is_hidden(object))
		return;
	if (lv_obj_check_type(object, &lv_image_class)) {
		struct apidemo_image_observation *image;
		const lv_image_dsc_t *source = lv_image_get_src(object);
		lv_obj_t *parent = lv_obj_get_parent(object);
		lv_obj_t *viewport = parent;

		assert(observation->image_count <
		       sizeof(observation->images) /
			       sizeof(observation->images[0]));
		while (lv_obj_get_parent(viewport) &&
		       !lv_obj_is_scrollable(viewport))
			viewport = lv_obj_get_parent(viewport);
		image = &observation->images[observation->image_count++];
		image->bounds = rectangle(object);
		image->frame = rectangle(parent);
		{
			lv_area_t content;

			lv_obj_get_content_coords(viewport, &content);
			image->viewport = (struct apidemo_rectangle){
				content.x1,
				content.y1,
				lv_area_get_width(&content),
				lv_area_get_height(&content),
			};
		}
		image->source_width = (int)source->header.w;
		image->source_height = (int)source->header.h;
	}
	if (menu && lv_obj_check_type(object, &lv_label_class)) {
		struct apidemo_menu_observation *row;
		lv_obj_t *icon = find_image(lv_obj_get_parent(object));

		assert(observation->menu_count <
		       sizeof(observation->menu) /
			       sizeof(observation->menu[0]));
		row = &observation->menu[observation->menu_count++];
		snprintf(row->text, sizeof(row->text), "%s",
			 lv_label_get_text(object));
		row->bounds = rectangle(object);
		row->icon = icon != NULL;
		if (icon) {
			const lv_draw_buf_t *source = lv_image_get_src(icon);

			assert(source->header.cf == LV_COLOR_FORMAT_A8);
			assert(source->header.w <= 24 &&
			       source->header.h <= 24);
			row->icon_width = (int)source->header.w;
			row->icon_height = (int)source->header.h;
			for (int y = 0; y < row->icon_height; ++y)
				memcpy(row->alpha + y * row->icon_width,
				       source->data + y * source->header.stride,
				       (size_t)row->icon_width);
		}
	}
	if (lv_obj_get_child_count(object) >= 2) {
		lv_obj_t *first = lv_obj_get_child(object, 0);
		lv_obj_t *second = lv_obj_get_child(object, 1);

		if (lv_obj_check_type(first, &lv_label_class) &&
		    lv_obj_check_type(second, &lv_label_class) &&
		    !strcmp(lv_label_get_text(first), ".") &&
		    !strcmp(lv_label_get_text(second), ","))
			observation->picker = rectangle(object);
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		collect(lv_obj_get_child(object, (int32_t)index), observation,
			menu);
}

bool __real_fplinux_ui_session_send(
	int fd, const struct fplinux_ui_session_message *message);

bool __wrap_fplinux_ui_session_send(
	int fd, const struct fplinux_ui_session_message *message)
{
	const char *descriptor = getenv("FPLINUX_APIDEMO_FRAME_FD");
	lv_display_t *display = lv_display_get_default();

	if (descriptor && application && display &&
	    message->type == FPLINUX_UI_SESSION_REDRAW_ACK) {
		struct apidemo_frame_observation observation = {
			.serial = message->serial,
		};

		collect(lv_display_get_screen_active(display), &observation,
			false);
		collect(lv_display_get_layer_top(display), &observation,
			fplinux_ui_menu_is_open(application));
		if (canvas_area && !lv_obj_is_hidden(canvas_area)) {
			observation.canvas = rectangle(canvas_area);
			for (uint32_t index = 0;
			     index < lv_obj_get_child_count(canvas_area);
			     ++index)
				collect_canvas(lv_obj_get_child(canvas_area,
								(int32_t)index),
					       -1, &observation);
		}
		assert(send(atoi(descriptor), &observation, sizeof(observation),
			    0) == (ssize_t)sizeof(observation));
	}
	return __real_fplinux_ui_session_send(fd, message);
}

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

struct fplinux_ui_screen *__real_fplinux_ui_text_box_new(struct fplinux_ui *ui,
							 const char *title,
							 const char *text,
							 size_t max_chars);

struct fplinux_ui_screen *__wrap_fplinux_ui_text_box_new(struct fplinux_ui *ui,
							 const char *title,
							 const char *text,
							 size_t max_chars)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_text_box_new(ui, title, text, max_chars);

	if (screen)
		observe(APIDEMO_TEXT_BOX, title, NULL);
	return screen;
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

	application = ui;
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

static void canvas_deleted(lv_event_t *event)
{
	if (lv_event_get_target_obj(event) == canvas_area)
		canvas_area = NULL;
}

struct fplinux_ui_screen *__wrap_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_canvas_new(ui, title);

	if (screen) {
		canvas_area = fplinux_ui_canvas_object(screen);
		lv_obj_add_event_cb(canvas_area, canvas_deleted,
				    LV_EVENT_DELETE, NULL);
		observe(APIDEMO_CANVAS, title, NULL);
	}
	return screen;
}
