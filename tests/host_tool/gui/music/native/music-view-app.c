/* SPDX-License-Identifier: GPL-2.0-only */
#include "music-view.h"
#include "fplinux-language.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui-system.h"

#include <assert.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_MUSIC_TEST_LANGUAGES");

	return __real_fplinux_language_load(languages ? languages : directory,
					    code, language);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_MUSIC_TEST_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

struct view_application {
	struct music_view view;
	unsigned int level_events;
};

static lv_obj_t *find_label(lv_obj_t *object, const char *text)
{
	if (!lv_obj_is_visible(object))
		return NULL;
	if (lv_obj_check_type(object, &lv_label_class) &&
	    !strcmp(lv_label_get_text(object), text))
		return object;
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
		lv_obj_t *label = find_label(lv_obj_get_child(object, i), text);

		if (label)
			return label;
	}
	return NULL;
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *message,
		  void *context)
{
	struct view_application *app = context;

	if (message->type == FPLINUX_UI_EVENT_CLOSE)
		fplinux_ui_quit(ui);
	if (message->type == FPLINUX_UI_EVENT_SYSTEM_LEVELS) {
		struct fplinux_system_level_state level;
		unsigned int update = app->level_events++;
		char full[96], compact[64];

		assert(update < 5 && !message->session);
		assert(fplinux_ui_system_level_get(
			ui, FPLINUX_SYSTEM_LEVEL_MEDIA, &level));
		music_view_update_volume(&app->view, ui, update == 4);
		if (update == 0 || update == 1 || update == 4) {
			unsigned int value = update == 0 ? 7 :
					     update == 1 ? 3 :
							   0;

			assert(level.availability ==
				       FPLINUX_SYSTEM_LEVEL_READY &&
			       level.value == value);
			snprintf(full, sizeof(full), gettext("Volume %u/%u"),
				 value, 10U);
			snprintf(compact, sizeof(compact), gettext("Vol %u/%u"),
				 value, 10U);
		} else {
			assert(level.availability ==
			       (update == 2 ? FPLINUX_SYSTEM_LEVEL_UNAVAILABLE :
					      FPLINUX_SYSTEM_LEVEL_UNKNOWN));
			snprintf(full, sizeof(full), "%s",
				 gettext(update == 2 ? "Volume unavailable" :
						       "Volume --"));
			snprintf(compact, sizeof(compact), "%s",
				 gettext(update == 2 ? "Vol n/a" : "Vol --"));
		}
		if (update == 4) {
			strcat(full, " !");
			strcat(compact, " !");
		}
		lv_obj_update_layout(fplinux_ui_screen(ui));
		lv_obj_t *status = find_label(fplinux_ui_screen(ui), full);
		lv_obj_t *title =
			find_label(fplinux_ui_screen(ui), gettext("Music"));
		lv_area_t status_box, title_box;

		if (!status)
			status = find_label(fplinux_ui_screen(ui), compact);
		assert(status && title);
		lv_obj_get_coords(status, &status_box);
		lv_obj_get_coords(title, &title_box);
		assert(status_box.x1 >= 0 &&
		       status_box.x2 < fplinux_ui_width(ui));
		assert(status_box.y1 >= 0 &&
		       status_box.y2 < fplinux_ui_height(ui));
		assert(title_box.x1 >= 0 && title_box.x2 < status_box.x1);
		struct fplinux_ui_session_message observed = {
			.type = FPLINUX_UI_SESSION_TITLE,
		};

		snprintf(observed.title, sizeof(observed.title), "level-%u",
			 update);
		assert(fplinux_ui_send(ui, &observed));
	}
}

static void assert_now_playing(const struct music_view *view, const char *state)
{
	const char title_prefix[] = "A long track";
	assert(lv_obj_is_visible(view->title));
	assert(!strncmp(lv_label_get_text(view->title), title_prefix,
			sizeof(title_prefix) - 1));
	assert(lv_obj_is_visible(view->state));
	assert(!strcmp(lv_label_get_text(view->state), state));
	assert(lv_obj_is_visible(view->time));
	assert(!strcmp(lv_label_get_text(view->time), "1:02 / 3:00"));
	assert(lv_obj_is_visible(view->progress));
	int64_t minimum = lv_bar_get_min_value(view->progress);
	int64_t range = lv_bar_get_max_value(view->progress) - minimum;
	int64_t filled = lv_bar_get_value(view->progress) - minimum;

	/* After 1:02 of a three-minute track, about a third is filled. */
	assert(range > 0 && 33 * range <= 100 * filled &&
	       100 * filled <= 35 * range);
}

static void assert_layout(const struct music_view *view, const char *state,
			  bool empty)
{
	lv_obj_t *canvas = fplinux_ui_canvas_object(view->screen);
	lv_obj_t *objects[] = { view->title, view->artist,   view->state,
				view->time,  view->progress, view->hint };
	int previous_bottom = 0;

	lv_obj_update_layout(canvas);
	int width = lv_obj_get_width(canvas);
	int height = lv_obj_get_height(canvas);
	assert(width > 0 && height > 0);
	if (!empty)
		assert_now_playing(view, state);
	for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
		lv_obj_t *object = objects[i];

		if (lv_obj_is_hidden(object))
			continue;
		int x = lv_obj_get_x(object), y = lv_obj_get_y(object);
		int right = x + lv_obj_get_width(object);
		int bottom = y + lv_obj_get_height(object);
		assert(x >= 0 && y >= previous_bottom);
		assert(right <= width && bottom <= height);
		previous_bottom = bottom;
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	struct view_application app = { 0 };
	struct fplinux_ui *ui =
		fplinux_ui_open("org.fplinux.music", event, &app);
	struct music_view *view = &app.view;
	assert(ui && music_view_create(view, ui));
	bool empty = !strcmp(argv[1], "empty");
	bool paused = !strcmp(argv[1], "paused");
	bool error = !strcmp(argv[1], "error");
	const char *state = gettext(error  ? "File unavailable" :
				    paused ? "Paused" :
					     "Playing");
	music_view_update(
		view,
		"A long track name that must leave time and progress visible",
		"Artist", state, 62000, 180000, true, empty);
	struct fplinux_ui_command action = {
		.id = 1,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = gettext(paused ? "Play" : "Pause"),
	};
	assert(fplinux_ui_screen_add_command(view->screen, &action));
	assert(fplinux_ui_push(ui, view->screen));
	music_view_layout(view, ui);
	assert_layout(view, state, empty);
	int result = fplinux_ui_run(ui);
	assert(app.level_events == 5);
	fplinux_ui_close(ui);
	return result;
}
