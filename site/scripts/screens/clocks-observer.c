/* SPDX-License-Identifier: GPL-2.0-only */
#include "clocks-observer.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Keep native localization deterministic without installing system catalogs. */
char *gettext(const char *text)
{
	if (getenv("FPLINUX_CLOCKS_TEST_TRANSLATIONS") &&
	    !strcmp(text, "Timer"))
		return "Таймер";
	return dgettext(NULL, text);
}

void __real_lv_display_flush_ready(lv_display_t *display);
struct fplinux_ui_screen *__real_fplinux_ui_form_new(struct fplinux_ui *ui,
						     const char *title);
void __real_fplinux_ui_form_delete_all(struct fplinux_ui_screen *form);
size_t __real_fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
					  const char *label,
					  enum fplinux_ui_date_mode mode,
					  time_t value);
size_t __real_fplinux_ui_form_append_duration(struct fplinux_ui_screen *form,
					      const char *label,
					      unsigned int seconds);

struct form_fields {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *form;
	size_t time_item;
	size_t date_item;
	size_t duration_item;
	unsigned int time_fields;
	unsigned int date_fields;
	unsigned int duration_fields;
};

static struct form_fields forms[16];
static size_t next_form;

static struct form_fields *find_form(struct fplinux_ui_screen *form)
{
	for (size_t index = 0; index < sizeof(forms) / sizeof(forms[0]);
	     ++index)
		if (forms[index].form == form)
			return &forms[index];
	return NULL;
}

struct fplinux_ui_screen *__wrap_fplinux_ui_form_new(struct fplinux_ui *ui,
						     const char *title)
{
	struct fplinux_ui_screen *form = __real_fplinux_ui_form_new(ui, title);
	struct form_fields *fields = find_form(form);

	if (!fields) {
		fields = &forms[next_form];
		next_form =
			(next_form + 1) % (sizeof(forms) / sizeof(forms[0]));
	}
	memset(fields, 0, sizeof(*fields));
	fields->ui = ui;
	fields->form = form;
	return form;
}

void __wrap_fplinux_ui_form_delete_all(struct fplinux_ui_screen *form)
{
	struct form_fields *fields = find_form(form);

	assert(fields);
	fields->time_fields = 0;
	fields->date_fields = 0;
	fields->duration_fields = 0;
	__real_fplinux_ui_form_delete_all(form);
}

size_t __wrap_fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
					  const char *label,
					  enum fplinux_ui_date_mode mode,
					  time_t value)
{
	size_t item =
		__real_fplinux_ui_form_append_date(form, label, mode, value);
	struct form_fields *fields = find_form(form);

	assert(fields);
	if (mode == FPLINUX_UI_TIME) {
		++fields->time_fields;
		fields->time_item = item;
	} else if (mode == FPLINUX_UI_DATE) {
		++fields->date_fields;
		fields->date_item = item;
	}
	return item;
}

size_t __wrap_fplinux_ui_form_append_duration(struct fplinux_ui_screen *form,
					      const char *label,
					      unsigned int seconds)
{
	size_t item =
		__real_fplinux_ui_form_append_duration(form, label, seconds);
	struct form_fields *fields = find_form(form);

	assert(fields);
	++fields->duration_fields;
	fields->duration_item = item;
	return item;
}

static void observe_fields(struct clocks_frame_observation *observation)
{
	struct form_fields *fields = NULL;

	for (size_t index = 0; index < sizeof(forms) / sizeof(forms[0]);
	     ++index)
		if (forms[index].ui &&
		    fplinux_ui_current(forms[index].ui) == forms[index].form) {
			fields = &forms[index];
			break;
		}
	if (!fields)
		return;
	observation->time_fields = fields->time_fields;
	observation->date_fields = fields->date_fields;
	observation->duration_fields = fields->duration_fields;
	observation->focused_item = fplinux_ui_form_get_focus(fields->form);
	observation->editing = fplinux_ui_form_is_editing(fields->form);
	if (fields->time_fields)
		observation->time_value = fplinux_ui_form_get_date(
			fields->form, fields->time_item);
	if (fields->date_fields) {
		if (fplinux_ui_form_is_hidden(fields->form, fields->date_item))
			observation->date_fields = 0;
		else
			observation->date_value = fplinux_ui_form_get_date(
				fields->form, fields->date_item);
	}
	if (fields->duration_fields)
		observation->duration_seconds = fplinux_ui_form_get_duration(
			fields->form, fields->duration_item);
}

static void measure_time(lv_obj_t *label, struct clocks_time_observation *time)
{
	const char *text = lv_label_get_text(label);
	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	lv_obj_t *parent = lv_obj_get_parent(label);
	lv_point_t size;

	if (!strchr(text, ':') || strlen(text) >= sizeof(time->text) ||
	    strspn(text, "0123456789:.") != strlen(text))
		return;
	assert(parent && font);
	for (size_t index = 0; text[index]; ++index) {
		lv_font_glyph_dsc_t glyph;

		assert(lv_font_get_glyph_dsc(font, &glyph,
					     (unsigned char)text[index],
					     (unsigned char)text[index + 1]));
	}
	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	if (size.y < time->text_height)
		return;
	strcpy(time->text, text);
	time->width = lv_obj_get_width(label);
	time->height = lv_obj_get_height(label);
	time->text_width = size.x;
	time->text_height = size.y;
	time->left = lv_obj_get_x(label);
	time->top = lv_obj_get_y(label);
	time->parent_width = lv_obj_get_width(parent);
	time->parent_height = lv_obj_get_height(parent);
}

static void collect_labels(lv_obj_t *object,
			   struct clocks_frame_observation *observation,
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
		measure_time(object, &observation->time);
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		collect_labels(lv_obj_get_child(object, (int32_t)index),
			       observation, used);
}

/* Observe the actual visible widget tree of each completed LVGL frame. */
void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *descriptor = getenv("FPLINUX_CLOCKS_OBSERVER_FD");
	struct clocks_frame_observation observation = {
		.labels = "\n",
		.focused_item = UINT64_MAX,
	};
	size_t used = 1;
	ssize_t sent;

	if (descriptor && lv_display_flush_is_last(display)) {
		observe_fields(&observation);
		collect_labels(lv_display_get_screen_active(display),
			       &observation, &used);
		collect_labels(lv_display_get_layer_top(display), &observation,
			       &used);
		collect_labels(lv_display_get_layer_sys(display), &observation,
			       &used);
		sent = send(atoi(descriptor), &observation, sizeof(observation),
			    MSG_DONTWAIT | MSG_NOSIGNAL);
		assert(sent == (ssize_t)sizeof(observation) ||
		       (sent == -1 &&
			(errno == EAGAIN || errno == EWOULDBLOCK)));
	}
	__real_lv_display_flush_ready(display);
}
