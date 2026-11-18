/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-language.h"
#include <fplinux/fplinux-ui.h>

#include <assert.h>
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_fsync(int fd);

int __wrap_fsync(int fd)
{
	const char *path = getenv("FPLINUX_APP_SAVE_FAULT");
	FILE *fault = path ? fopen(path, "r") : NULL;
	struct stat status;
	int operation;

	if (fault) {
		operation = fgetc(fault);
		assert(fclose(fault) == 0 && fstat(fd, &status) == 0);
		if ((operation == 'f' && S_ISREG(status.st_mode)) ||
		    (operation == 'd' && S_ISDIR(status.st_mode))) {
			errno = EIO;
			return -1;
		}
	}
	return __real_fsync(fd);
}

static unsigned int count_images(lv_obj_t *object)
{
	unsigned int images = lv_obj_check_type(object, &lv_image_class);

	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
		images += count_images(lv_obj_get_child(object, (int32_t)i));
	return images;
}

static void observe_label(lv_obj_t *label, struct json_object *labels)
{
	lv_obj_t *row = lv_obj_get_parent(label);
	lv_area_t bounds, parent;
	lv_point_t natural;
	struct json_object *entry = json_object_new_object();
	const char *text = lv_label_get_text(label);

	assert(entry && row);
	lv_obj_get_coords(label, &bounds);
	lv_obj_get_coords(row, &parent);
	lv_text_get_size(
		&natural, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
		lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
		LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	json_object_object_add(entry, "text", json_object_new_string(text));
	json_object_object_add(entry, "natural_width",
			       json_object_new_int(natural.x));
	json_object_object_add(entry, "width",
			       json_object_new_int(lv_area_get_width(&bounds)));
	json_object_object_add(entry, "left", json_object_new_int(bounds.x1));
	json_object_object_add(entry, "right", json_object_new_int(bounds.x2));
	json_object_object_add(entry, "top", json_object_new_int(bounds.y1));
	json_object_object_add(entry, "bottom", json_object_new_int(bounds.y2));
	json_object_object_add(entry, "row_top",
			       json_object_new_int(parent.y1));
	json_object_object_add(entry, "row_bottom",
			       json_object_new_int(parent.y2));
	json_object_object_add(
		entry, "row_height",
		json_object_new_int(lv_area_get_height(&parent)));
	json_object_object_add(entry, "images",
			       json_object_new_int(count_images(row)));
	json_object_array_add(labels, entry);
}

static void collect_labels(lv_obj_t *object, FILE *output,
			   struct json_object *labels)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_label_class)) {
		observe_label(object, labels);
		for (const char *text = lv_label_get_text(object); *text;
		     text++)
			fputc(*text == '\n' ? ' ' : *text, output);
		fputc('|', output);
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     index++)
		collect_labels(lv_obj_get_child(object, (int32_t)index), output,
			       labels);
}

void __real_lv_display_flush_ready(lv_display_t *display);

/* Observe rendered labels; the runtime still presents the real Wayland frame. */
void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *path = getenv("FPLINUX_APP_FRAME_RECORD");

	if (path && lv_display_flush_is_last(display)) {
		FILE *output = fopen(path, "a");
		struct json_object *labels = json_object_new_array();
		char layout[4096];

		assert(output && labels);
		collect_labels(lv_display_get_screen_active(display), output,
			       labels);
		collect_labels(lv_display_get_layer_top(display), output,
			       labels);
		collect_labels(lv_display_get_layer_sys(display), output,
			       labels);
		fputc('\n', output);
		assert(fclose(output) == 0);
		assert(snprintf(layout, sizeof(layout), "%s.layout.json",
				path) < (int)sizeof(layout));
		output = fopen(layout, "w");
		assert(output && fputs(json_object_to_json_string_ext(
					       labels, JSON_C_TO_STRING_PLAIN),
				       output) >= 0);
		assert(fclose(output) == 0);
		json_object_put(labels);
	}
	__real_lv_display_flush_ready(display);
}

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

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_UI_LOCALE_DIRECTORY");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}
