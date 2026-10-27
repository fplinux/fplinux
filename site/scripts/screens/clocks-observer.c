/* SPDX-License-Identifier: GPL-2.0-only */
#include "clocks-observer.h"
#include "fplinux-language.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static void *application_context;
static lv_timer_t *application_timer;
static lv_timer_cb_t application_tick;
static unsigned int poll_callbacks;
static unsigned int poll_period_ms;

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_CLOCKS_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_CLOCKS_LANGUAGE_DIR");

	return __real_fplinux_language_load(
		languages && !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY) ?
			languages :
			directory,
		code, language);
}

struct fplinux_ui *__real_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context);
lv_timer_t *__real_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context);
void __real_lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void __real_lv_timer_delete(lv_timer_t *timer);

static void send_observation(struct clocks_frame_observation *observation)
{
	const char *descriptor = getenv("FPLINUX_CLOCKS_OBSERVER_FD");
	ssize_t sent;

	if (!descriptor)
		return;
	observation->poll_callbacks = poll_callbacks;
	observation->poll_period_ms = poll_period_ms;
	observation->poll_paused = application_timer &&
				   lv_timer_get_paused(application_timer);
	sent = send(atoi(descriptor), observation, sizeof(*observation),
		    MSG_DONTWAIT | MSG_NOSIGNAL);
	assert(sent == (ssize_t)sizeof(*observation) ||
	       (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));
}

static void observe_application_tick(lv_timer_t *timer)
{
	struct clocks_frame_observation observation = { .timer_only = true };

	++poll_callbacks;
	application_tick(timer);
	send_observation(&observation);
}

struct fplinux_ui *__wrap_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_context = context;
	return __real_fplinux_ui_open(app_id, callback, context);
}

/* Observe the timer owned by the application's lifecycle context. */
lv_timer_t *__wrap_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context)
{
	if (context != application_context)
		return __real_lv_timer_create(callback, period, context);
	assert(!application_timer);
	application_tick = callback;
	poll_period_ms = period;
	application_timer = __real_lv_timer_create(observe_application_tick,
						   period, context);
	return application_timer;
}

void __wrap_lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
	if (timer == application_timer)
		poll_period_ms = period;
	__real_lv_timer_set_period(timer, period);
}

void __wrap_lv_timer_delete(lv_timer_t *timer)
{
	if (timer == application_timer)
		application_timer = NULL;
	__real_lv_timer_delete(timer);
}

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

static void measure_time(lv_obj_t *label, struct clocks_time_observation *time,
			 bool fraction)
{
	const char *text = lv_label_get_text(label);
	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	lv_obj_t *parent = lv_obj_get_parent(label);
	lv_point_t size;

	if ((fraction ? text[0] != '.' : !strchr(text, ':')) ||
	    strlen(text) >= sizeof(time->text) ||
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

static void measure_alarm_row(lv_obj_t *label,
			      struct clocks_frame_observation *observation)
{
	const char *text = lv_label_get_text(label);
	lv_obj_t *column = lv_obj_get_parent(label);
	lv_obj_t *row = column ? lv_obj_get_parent(column) : NULL;
	lv_area_t primary, row_bounds;

	if (strlen(text) != 5 || text[2] != ':' ||
	    strspn(text, "0123456789:") != 5 || !row)
		return;
	lv_obj_get_coords(label, &primary);
	lv_obj_get_coords(row, &row_bounds);
	observation->alarm_primary_left = primary.x1;
	observation->alarm_primary_right = primary.x2;
	observation->alarm_primary_top = primary.y1;
	observation->alarm_primary_bottom = primary.y2;
	observation->alarm_row_left = row_bounds.x1;
	observation->alarm_row_right = row_bounds.x2;
	observation->alarm_row_top = row_bounds.y1;
	observation->alarm_row_bottom = row_bounds.y2;
	for (uint32_t index = 0; index < lv_obj_get_child_count(column);
	     ++index) {
		lv_obj_t *detail = lv_obj_get_child(column, (int32_t)index);

		if (detail != label &&
		    lv_obj_check_type(detail, &lv_label_class))
			observation->alarm_detail_height =
				lv_font_get_line_height(
					lv_obj_get_style_text_font(
						detail, LV_PART_MAIN));
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(row); ++index) {
		lv_obj_t *image = lv_obj_get_child(row, (int32_t)index);
		lv_area_t status;

		if (!lv_obj_check_type(image, &lv_image_class) ||
		    !lv_obj_is_visible(image))
			continue;
		lv_obj_get_coords(image, &status);
		observation->alarm_status_left = status.x1;
		observation->alarm_status_right = status.x2;
		observation->alarm_status_top = status.y1;
		observation->alarm_status_bottom = status.y2;
		observation->alarm_status_found = true;
		break;
	}
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
		static const char *const roots[4][2] = {
			{ "Alarms", "Будильники" },
			{ "Timer", "Таймер" },
			{ "Stopwatch", "Секундомер" },
			{ "World clock", "Мировое время" },
		};

		for (size_t index = 0; index < 4; ++index)
			if (!strcmp(text, roots[index][0]) ||
			    !strcmp(text, roots[index][1]))
				screen_observe_list_value(
					object,
					&observation->root_values[index]);

		assert(*used + length + 2 < sizeof(observation->labels));
		memcpy(observation->labels + *used, text, length);
		*used += length;
		observation->labels[(*used)++] = '\n';
		observation->labels[*used] = '\0';
		measure_time(object, &observation->time, false);
		measure_time(object, &observation->fraction, true);
		measure_alarm_row(object, observation);
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		collect_labels(lv_obj_get_child(object, (int32_t)index),
			       observation, used);
}

/* Observe the actual visible widget tree of each completed LVGL frame. */
void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	struct clocks_frame_observation observation = {
		.labels = "\n",
		.focused_item = UINT64_MAX,
	};
	size_t used = 1;

	if (getenv("FPLINUX_CLOCKS_OBSERVER_FD") &&
	    lv_display_flush_is_last(display)) {
		observe_fields(&observation);
		collect_labels(lv_display_get_screen_active(display),
			       &observation, &used);
		collect_labels(lv_display_get_layer_top(display), &observation,
			       &used);
		collect_labels(lv_display_get_layer_sys(display), &observation,
			       &used);
		send_observation(&observation);
	}
	__real_lv_display_flush_ready(display);
}
