/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "launcher-observer.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-language.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static void *application_context;
static fplinux_ui_event_cb application_event;
static lv_timer_t *clock_timer;
static lv_timer_cb_t clock_tick;
static unsigned int clock_callbacks;
static unsigned int clock_period_ms;
static uint32_t active_serial;

struct fplinux_ui *__real_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context);
lv_timer_t *__real_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context);
void __real_lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void __real_lv_timer_delete(lv_timer_t *timer);
void __real_lv_display_flush_ready(lv_display_t *display);

static void send_observation(struct launcher_frame_observation *observation)
{
	const char *descriptor = getenv("FPLINUX_LAUNCHER_OBSERVER_FD");
	ssize_t sent;

	if (!descriptor)
		return;
	observation->clock_callbacks = clock_callbacks;
	observation->clock_period_ms = clock_period_ms;
	observation->clock_paused = clock_timer &&
				    lv_timer_get_paused(clock_timer);
	observation->animations = lv_anim_count_running();
	observation->active_serial = active_serial;
	sent = send(atoi(descriptor), observation, sizeof(*observation),
		    MSG_DONTWAIT | MSG_NOSIGNAL);
	assert(sent == (ssize_t)sizeof(*observation) ||
	       (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));
}

static void observe_tick(lv_timer_t *timer)
{
	struct launcher_frame_observation observation = { .timer_only = true };

	++clock_callbacks;
	clock_tick(timer);
	send_observation(&observation);
}

static void observe_event(struct fplinux_ui *ui,
			  const struct fplinux_ui_event *event, void *context)
{
	struct launcher_frame_observation observation = { .timer_only = true };

	if (event->type == FPLINUX_UI_EVENT_SESSION && event->session &&
	    event->session->type == FPLINUX_UI_SESSION_ACTIVE)
		active_serial = event->session->serial;
	application_event(ui, event, context);
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		send_observation(&observation);
}

struct fplinux_ui *__wrap_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_context = context;
	application_event = callback;
	return __real_fplinux_ui_open(app_id, observe_event, context);
}

lv_timer_t *__wrap_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context)
{
	if (context != application_context || period != 60000)
		return __real_lv_timer_create(callback, period, context);
	assert(!clock_timer);
	clock_tick = callback;
	clock_period_ms = period;
	clock_timer = __real_lv_timer_create(observe_tick, period, context);
	return clock_timer;
}

void __wrap_lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
	if (timer == clock_timer)
		clock_period_ms = period;
	__real_lv_timer_set_period(timer, period);
}

void __wrap_lv_timer_delete(lv_timer_t *timer)
{
	if (timer == clock_timer)
		clock_timer = NULL;
	__real_lv_timer_delete(timer);
}

static void collect(lv_obj_t *object,
		    struct launcher_frame_observation *observation)
{
	int extent;

	if (!lv_obj_is_visible(object))
		return;
	extent = lv_obj_get_scroll_top(object) +
		 lv_obj_get_scroll_bottom(object);
	if (extent > observation->scroll_extent)
		observation->scroll_extent = extent;
	if (lv_obj_check_type(object, &lv_line_class))
		++observation->lines;
	if (lv_obj_check_type(object, &lv_label_class)) {
		struct launcher_label_observation *label;
		lv_area_t bounds;
		lv_point_t size;

		assert(observation->label_count < 32);
		label = &observation->labels[observation->label_count++];
		snprintf(label->text, sizeof(label->text), "%s",
			 lv_label_get_text(object));
		lv_obj_get_coords(object, &bounds);
		label->x = bounds.x1;
		label->y = bounds.y1;
		label->width = lv_area_get_width(&bounds);
		label->height = lv_area_get_height(&bounds);
		lv_text_get_size(&size, label->text,
				 lv_obj_get_style_text_font(object,
							    LV_PART_MAIN),
				 0, 0, label->width, LV_TEXT_FLAG_NONE);
		label->text_width = size.x;
		label->text_height = size.y;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
		collect(lv_obj_get_child(object, (int32_t)i), observation);
}

static void collect_lines(lv_obj_t *object,
			  struct launcher_frame_observation *observation)
{
	if (lv_obj_check_type(object, &lv_line_class)) {
		struct launcher_line_observation *line;
		lv_obj_t *parent = lv_obj_get_parent(object);
		lv_area_t bounds;

		assert(observation->line_objects_count < 8);
		line = &observation
				->line_objects[observation->line_objects_count++];
		line->visible = lv_obj_is_visible(object);
		line->points = lv_line_get_point_count(object);
		lv_obj_get_coords(object, &bounds);
		line->bounds[0] = bounds.x1;
		line->bounds[1] = bounds.y1;
		line->bounds[2] = lv_area_get_width(&bounds);
		line->bounds[3] = lv_area_get_height(&bounds);
		if (parent) {
			lv_obj_get_coords(parent, &bounds);
			line->parent_bounds[0] = bounds.x1;
			line->parent_bounds[1] = bounds.y1;
			line->parent_bounds[2] = lv_area_get_width(&bounds);
			line->parent_bounds[3] = lv_area_get_height(&bounds);
			line->parent_radius =
				lv_obj_get_style_radius(parent, LV_PART_MAIN);
		}
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
		collect_lines(lv_obj_get_child(object, (int32_t)i),
			      observation);
}

void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	struct launcher_frame_observation observation = { 0 };

	if (getenv("FPLINUX_LAUNCHER_OBSERVER_FD") &&
	    lv_display_flush_is_last(display)) {
		collect(lv_display_get_screen_active(display), &observation);
		collect(lv_display_get_layer_top(display), &observation);
		collect(lv_display_get_layer_sys(display), &observation);
		collect_lines(lv_display_get_screen_active(display),
			      &observation);
		collect_lines(lv_display_get_layer_top(display), &observation);
		collect_lines(lv_display_get_layer_sys(display), &observation);
		send_observation(&observation);
	}
	__real_lv_display_flush_ready(display);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_UI_LOCALE_DIRECTORY");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_LAUNCHER_LANGUAGE_DIRECTORY");

	return __real_fplinux_language_load(languages ? languages : directory,
					    code, language);
}

time_t __real_time(time_t *result);

time_t __wrap_time(time_t *result)
{
	if (!getenv("FPLINUX_LAUNCHER_TEST_CLOCK"))
		return __real_time(result);
	if (result)
		*result = 1793084400;
	return 1793084400;
}

int __real_clock_gettime(clockid_t clock, struct timespec *result);

int __wrap_clock_gettime(clockid_t clock, struct timespec *result)
{
	if (clock != CLOCK_REALTIME || !getenv("FPLINUX_LAUNCHER_TEST_CLOCK"))
		return __real_clock_gettime(clock, result);
	*result = (struct timespec){ .tv_sec = 1793084400 };
	return 0;
}
