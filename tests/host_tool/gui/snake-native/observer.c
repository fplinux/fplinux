/* SPDX-License-Identifier: GPL-2.0-only */
#include "observer.h"
#include "fplinux-language.h"
#include "fplinux-ui-profile.h"
#include "snake.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>
#include <assert.h>
#include <libintl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct fplinux_ui *application_ui;
static struct fplinux_ui_screen *canvas;
static struct fplinux_snake *model;
static unsigned int frames;
static unsigned int canvases;
static unsigned int starts;

const struct fplinux_ui_profile_spec *
__real_fplinux_ui_profile_spec(int profile);

const struct fplinux_ui_profile_spec *
__wrap_fplinux_ui_profile_spec(int profile)
{
	const struct fplinux_ui_profile_spec *actual =
		__real_fplinux_ui_profile_spec(profile);
	const char *fixture = getenv("FPLINUX_NATIVE_DIVIDER");
	static struct fplinux_ui_profile_spec changed;

	if (!fixture)
		return actual;
	assert(actual && !strcmp(fixture, "3"));
	changed = *actual;
	changed.divider_width = 3;
	return &changed;
}

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

struct fplinux_ui_screen *__real_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title);

struct fplinux_ui_screen *__wrap_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title)
{
	assert(!title);
	canvas = __real_fplinux_ui_canvas_new(ui, title);
	++canvases;
	return canvas;
}

void __real_fplinux_snake_start(struct fplinux_snake *snake, uint32_t seed,
				unsigned int columns, unsigned int rows);

void __wrap_fplinux_snake_start(struct fplinux_snake *snake, uint32_t seed,
				unsigned int columns, unsigned int rows)
{
	model = snake;
	++starts;
	__real_fplinux_snake_start(snake, seed, columns, rows);
}

bool __real_fplinux_snake_tick(struct fplinux_snake *snake);

/* A controlled win exercises its screen route; the model has its own tests. */
bool __wrap_fplinux_snake_tick(struct fplinux_snake *snake)
{
	const char *descriptor = getenv("FPLINUX_SNAKE_OBSERVER_FD");
	char outcome;

	if (descriptor &&
	    recv(atoi(descriptor), &outcome, 1, MSG_DONTWAIT) == 1) {
		assert(outcome == 'w');
		snake->won = true;
		snake->paused = true;
		return true;
	}
	return __real_fplinux_snake_tick(snake);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_SNAKE_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

static void collect_labels(lv_obj_t *object,
			   struct snake_observation *observation, size_t *used)
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

void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	const char *descriptor = getenv("FPLINUX_SNAKE_OBSERVER_FD");
	struct snake_observation observation = { .labels = "\n" };
	size_t used = 1;

	if (descriptor && application_ui && model &&
	    lv_display_flush_is_last(display)) {
		observation.frame = ++frames;
		observation.canvases = canvases;
		observation.starts = starts;
		observation.head = model->body[0];
		observation.pending = model->pending;
		observation.score = model->score;
		observation.canvas = fplinux_ui_current(application_ui) ==
				     canvas;
		observation.paused = model->paused;
		observation.dead = model->dead;
		observation.won = model->won;
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
