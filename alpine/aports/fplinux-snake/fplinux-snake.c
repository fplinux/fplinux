/* SPDX-License-Identifier: GPL-2.0-only */
#include "snake.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-gettext.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "org.fplinux.snake"
#define TICK_MS 125
/* About as many cells across every screen; the rows fill the height. */
#define CELLS_ACROSS 20
#define _(text) gettext(text)
#define NC_(context, text) context "\004" text

enum command {
	COMMAND_PLAY,
	COMMAND_BACK,
};

struct snake_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_snake snake;
	struct fplinux_ui_screen *canvas;
	struct fplinux_ui_screen *alert;
	lv_obj_t *drawing;
	lv_obj_t *board;
	lv_obj_t *score;
	lv_obj_t *best;
	/* Fullscreen Canvas keeps its bar below runtime-owned modal legends. */
	lv_obj_t *softkey_bar;
	lv_timer_t *timer;
	int cell;
	unsigned int previous_score;
	/* The score shows only its number: both labels do not fit in full. */
	bool short_score;
	bool played;
	bool screen_kept_on;
	bool hidden;
	bool closing;
	struct fplinux_storage_result saved;
};

static void refresh(struct snake_app *app);
static void show_pause(struct snake_app *app);

/* The preferences keep the scores in memory until they are saved. */
static bool store_scores(struct snake_app *app)
{
	return fplinux_ui_prefs_set_int(app->prefs, "last_score",
					app->played ? app->snake.score :
						      app->previous_score) &&
	       fplinux_ui_prefs_set_int(app->prefs, "best", app->snake.best);
}

static void save_scores(struct snake_app *app)
{
	struct fplinux_storage_result result;

	if (!store_scores(app)) {
		result.state = FPLINUX_STORAGE_UNPUBLISHED;
		result.error = errno;
	} else {
		result = fplinux_ui_prefs_save(app->prefs);
	}
	if (result.state != FPLINUX_STORAGE_DURABLE &&
	    (app->saved.state != result.state ||
	     app->saved.error != result.error)) {
		errno = result.error;
		perror("fplinux-snake: save scores");
	} else if (result.state == FPLINUX_STORAGE_DURABLE &&
		   app->saved.error) {
		fputs("fplinux-snake: scores saved\n", stderr);
	}
	app->saved = result;
	if (app->alert)
		show_pause(app);
}

static void load_scores(struct snake_app *app)
{
	long long score = fplinux_ui_prefs_get_int(app->prefs, "last_score", 0);
	long long best = fplinux_ui_prefs_get_int(app->prefs, "best", 0);

	if (score >= 0 && score <= FPLINUX_SNAKE_MAX_CELLS - 3 && best >= 0 &&
	    best <= FPLINUX_SNAKE_MAX_CELLS - 3) {
		app->previous_score = (unsigned int)score;
		app->snake.best = (unsigned int)(best > score ? best : score);
	}
}

/*
 * A square of the cell less 1 px, so that neighbours keep a 1 px gap, and
 * less inset on every side.
 */
static void draw_cell(struct snake_app *app, lv_layer_t *layer,
		      const lv_area_t *bounds, unsigned int cell, int inset,
		      lv_color_t color)
{
	lv_draw_rect_dsc_t rectangle;
	lv_area_t area;
	int size = app->cell;

	area.x1 = bounds->x1 + (int)(cell % app->snake.columns) * size + inset;
	area.y1 = bounds->y1 + (int)(cell / app->snake.columns) * size + inset;
	area.x2 = area.x1 + size - 2 - 2 * inset;
	area.y2 = area.y1 + size - 2 - 2 * inset;
	lv_draw_rect_dsc_init(&rectangle);
	rectangle.bg_color = color;
	rectangle.bg_opa = LV_OPA_COVER;
	lv_draw_rect(layer, &rectangle, &area);
}

static void draw_board(lv_event_t *event)
{
	struct snake_app *app = lv_event_get_user_data(event);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	lv_layer_t *layer = lv_event_get_layer(event);
	lv_area_t bounds;
	unsigned int segment;

	lv_obj_get_coords(app->board, &bounds);
	/*
	 * The food is clearly smaller than a segment (a sixth of the cell
	 * off each side, at least 1 px), so it differs by shape too.
	 */
	if (!app->snake.won)
		draw_cell(app, layer, &bounds, app->snake.food,
			  app->cell / 6 > 1 ? app->cell / 6 : 1, theme->danger);
	for (segment = 0; segment < app->snake.length; segment++)
		draw_cell(app, layer, &bounds, app->snake.body[segment], 0,
			  segment ? theme->focus : theme->foreground);
}

static const char *play_label(const struct snake_app *app)
{
	if (!app->snake.paused)
		return _("Pause");
	if (app->snake.dead || app->snake.won)
		return _("Restart");
	return app->played ? _("Resume") : _("Play");
}

static struct fplinux_ui_command play_command(const struct snake_app *app)
{
	return (struct fplinux_ui_command){
		.id = COMMAND_PLAY,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = play_label(app),
		.short_label = app->snake.paused && app->played &&
					       !app->snake.dead &&
					       !app->snake.won ?
				       fplinux_gettext_context(
					       NC_("short command", "Resume")) :
				       NULL,
	};
}

static void refresh(struct snake_app *app)
{
	struct fplinux_ui_command play = play_command(app);
	bool playing = !app->snake.paused && !app->hidden && !app->closing;
	char message[64];

	if (!app->canvas)
		return;

	if (app->short_score)
		snprintf(message, sizeof(message), "%u", app->snake.score);
	else
		snprintf(message, sizeof(message), _("Score %u"),
			 app->snake.score);
	lv_label_set_text(app->score, message);
	snprintf(message, sizeof(message), _("Best %u"), app->snake.best);
	lv_label_set_text(app->best, message);
	fplinux_ui_screen_add_command(app->canvas, &play);
	fplinux_ui_softkey_bar_set(
		app->ui, app->softkey_bar, "",
		play.short_label ? play.short_label : play.label, _("Back"));
	lv_obj_invalidate(app->board);
	if (playing)
		lv_timer_resume(app->timer);
	else
		lv_timer_pause(app->timer);
	/* A game is watched without pressing keys: keep the display on. */
	if (playing != app->screen_kept_on &&
	    fplinux_ui_keep_screen_on(app->ui, playing))
		app->screen_kept_on = playing;
}

static void play(struct snake_app *app)
{
	if (app->closing || app->hidden)
		return;
	if (app->snake.dead || app->snake.won) {
		app->previous_score = app->snake.score;
		fplinux_snake_start(&app->snake, fplinux_ui_now_ms(),
				    app->snake.columns, app->snake.rows);
	}
	app->snake.paused = !app->snake.paused;
	app->played = true;
	lv_timer_reset(app->timer);
	refresh(app);
	if (app->snake.paused)
		show_pause(app);
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int command_id, void *context)
{
	struct snake_app *app = context;

	/* Alert commands have already returned to the permanent Canvas. */
	if (screen == app->alert)
		app->alert = NULL;
	if (command_id == COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_PLAY)
		play(app);
}

static bool shortcut_key(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen,
			 const struct fplinux_ui_key *key, void *context)
{
	struct snake_app *app = context;

	if (key->code == KEY_HOME) {
		if (key->pressed)
			fplinux_ui_home(ui);
		return true;
	}
	if (key->code != KEY_SPACE)
		return false;
	if (!key->pressed)
		return true;
	if (screen == app->alert) {
		app->alert = NULL;
		fplinux_ui_pop(ui);
	}
	play(app);
	return true;
}

static void show_pause(struct snake_app *app)
{
	struct fplinux_ui_command play = play_command(app);
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Back"),
	};
	const char *title;
	char message[256];

	if (app->closing || !app->canvas || !app->snake.paused)
		return;
	if (app->snake.dead || app->snake.won) {
		title = app->snake.dead ? _("Game over") : _("✓ You won!");
		snprintf(message, sizeof(message),
			 app->snake.won ? _("Board complete!\nScore %u") :
					  _("Score %u"),
			 app->snake.score);
	} else {
		title = app->played ? _("Paused") : _("Snake");
		snprintf(message, sizeof(message),
			 _("Arrows / 2/4/6/8\nLast score %u"),
			 app->previous_score);
	}
	if (app->saved.state != FPLINUX_STORAGE_DURABLE) {
		size_t used = strlen(message);

		snprintf(message + used, sizeof(message) - used, "\n%s\n%s",
			 app->saved.state ==
					 FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				 _("Score saving not confirmed") :
				 _("Scores not saved"),
			 fplinux_ui_storage_error(app->saved));
	}
	if (app->alert) {
		fplinux_ui_alert_set_text(app->alert, message);
		return;
	}
	app->alert = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO, title,
					  message, 0);
	if (!app->alert)
		return;
	if (!fplinux_ui_screen_add_command(app->alert, &play) ||
	    !fplinux_ui_screen_add_command(app->alert, &back)) {
		fplinux_ui_push(app->ui, app->alert);
		fplinux_ui_pop(app->ui);
		app->alert = NULL;
		return;
	}
	fplinux_ui_screen_set_listener(app->alert, command, NULL, app);
	fplinux_ui_screen_set_key_filter(app->alert, shortcut_key, app);
	fplinux_ui_push(app->ui, app->alert);
}

static void timer(lv_timer_t *timer)
{
	struct snake_app *app = lv_timer_get_user_data(timer);

	if (fplinux_snake_tick(&app->snake)) {
		refresh(app);
		if (app->snake.dead || app->snake.won)
			store_scores(app);
		if (app->snake.paused)
			show_pause(app);
	}
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	struct snake_app *app = context;
	(void)ui;

	if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
	    event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		app->snake.paused = true;
		refresh(app);
		show_pause(app);
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		app->closing = true;
		/* CLOSE can arrive before fplinux_ui_open() returns. */
		if (app->timer) {
			app->snake.paused = true;
			refresh(app);
		}
		save_scores(app);
		return;
	}
	/* Hidden by the display turning off or the lock: pause and save. */
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY) {
		app->hidden = !event->visible;
		if (!event->visible) {
			app->snake.paused = true;
			refresh(app);
			save_scores(app);
		} else {
			show_pause(app);
		}
	}
}

static void direction(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		      const struct fplinux_ui_key *key, void *context)
{
	struct snake_app *app = context;
	unsigned int code = key->code;

	(void)ui;
	(void)canvas;
	if (!key->pressed || app->snake.paused || app->hidden || app->closing)
		return;
	if (code == KEY_UP || code == KEY_NUMERIC_2 || code == KEY_2 ||
	    code == KEY_KP2)
		fplinux_snake_turn(&app->snake, FPLINUX_SNAKE_UP);
	else if (code == KEY_DOWN || code == KEY_NUMERIC_8 || code == KEY_8 ||
		 code == KEY_KP8)
		fplinux_snake_turn(&app->snake, FPLINUX_SNAKE_DOWN);
	else if (code == KEY_LEFT || code == KEY_NUMERIC_4 || code == KEY_4 ||
		 code == KEY_KP4)
		fplinux_snake_turn(&app->snake, FPLINUX_SNAKE_LEFT);
	else if (code == KEY_RIGHT || code == KEY_NUMERIC_6 || code == KEY_6 ||
		 code == KEY_KP6)
		fplinux_snake_turn(&app->snake, FPLINUX_SNAKE_RIGHT);
}

static lv_obj_t *create_bar(struct snake_app *app, int y, int height,
			    lv_border_side_t divider)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	lv_obj_t *bar = lv_obj_create(app->drawing);

	lv_obj_remove_style_all(bar);
	lv_obj_set_pos(bar, 0, y);
	lv_obj_set_size(bar, fplinux_ui_width(app->ui), height);
	lv_obj_set_style_bg_color(bar, theme->bar, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(bar, theme->border, 0);
	lv_obj_set_style_border_width(
		bar, fplinux_ui_metrics(app->ui)->divider_width, 0);
	lv_obj_set_style_border_side(bar, divider, 0);
	return bar;
}

/* One line high, in the bar colours; the caller places it on a bar. */
static lv_obj_t *create_bar_label(struct snake_app *app, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(app->drawing);

	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, fplinux_ui_theme(app->ui)->bar_text,
				    0);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

static int32_t text_width(const lv_font_t *font, const char *format,
			  unsigned int value)
{
	char text[64];
	lv_point_t size;

	snprintf(text, sizeof(text), format, value);
	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	return size.x;
}

/* Status and soft-key bars surround a board of whole cells. */
static bool create_view(struct snake_app *app)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	int width;
	int top = metrics->status_height;
	int bottom;
	int line = lv_font_get_line_height(theme->font);
	unsigned int most = FPLINUX_SNAKE_MAX_CELLS - 3;
	int board_width;
	int board_height;
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Back"),
	};

	app->canvas = fplinux_ui_canvas_new(app->ui, NULL);
	if (!app->canvas)
		return false;
	fplinux_ui_screen_add_command(app->canvas, &back);
	fplinux_ui_screen_set_listener(app->canvas, command, NULL, app);
	fplinux_ui_screen_set_key_filter(app->canvas, shortcut_key, app);
	fplinux_ui_canvas_set_key_handler(app->canvas, direction, app);
	fplinux_ui_push(app->ui, app->canvas);
	app->drawing = fplinux_ui_canvas_object(app->canvas);
	lv_obj_update_layout(app->drawing);
	width = lv_obj_get_width(app->drawing);
	bottom = lv_obj_get_height(app->drawing) - metrics->softkey_height;

	app->cell = width / CELLS_ACROSS > 1 ? width / CELLS_ACROSS : 1;
	fplinux_snake_start(&app->snake, fplinux_ui_now_ms(),
			    (unsigned int)(width / app->cell),
			    (unsigned int)((bottom - top) / app->cell));
	board_width = (int)app->snake.columns * app->cell;
	board_height = (int)app->snake.rows * app->cell;
	create_bar(app, 0, top, LV_BORDER_SIDE_BOTTOM);
	app->softkey_bar = fplinux_ui_softkey_bar_create(app->ui, app->drawing);

	app->board = lv_obj_create(app->drawing);
	lv_obj_remove_style_all(app->board);
	lv_obj_set_scrollable(app->board, false);
	lv_obj_set_pos(app->board, (width - board_width) / 2,
		       top + (bottom - top - board_height) / 2);
	lv_obj_set_size(app->board, board_width, board_height);
	lv_obj_set_style_bg_color(app->board, theme->surface, 0);
	lv_obj_set_style_bg_opa(app->board, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(app->board, draw_board, LV_EVENT_DRAW_MAIN, app);

	/* Both scores in full when the widest ones fit side by side. */
	app->short_score = text_width(theme->font, _("Score %u"), most) +
				   metrics->spacing +
				   text_width(theme->font, _("Best %u"), most) >
			   width - 2 * metrics->margin;
	app->score = create_bar_label(app, theme->font);
	lv_obj_set_pos(app->score, metrics->margin, (top - line) / 2);
	app->best = create_bar_label(app, theme->font);
	lv_obj_align(app->best, LV_ALIGN_TOP_RIGHT, -metrics->margin,
		     (top - line) / 2);
	/* OK plays and pauses; the left soft key has no action. */
	app->timer = lv_timer_create(timer, TICK_MS, app);
	refresh(app);
	show_pause(app);
	return true;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-snake",
		.description = "Play Snake with the phone keypad",
	};
	struct snake_app app = {
		.saved = { .state = FPLINUX_STORAGE_DURABLE },
	};
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	app.prefs = fplinux_ui_prefs_open(APP_ID);
	if (!app.prefs) {
		perror("fplinux-snake: preferences");
		return 1;
	}
	load_scores(&app);
	app.ui = fplinux_ui_open(APP_ID, event, &app);
	if (!app.ui) {
		fplinux_ui_prefs_close(app.prefs);
		return 1;
	}
	if (!app.closing && !create_view(&app))
		result = -1;
	else
		result = fplinux_ui_run(app.ui);
	app.closing = true;
	save_scores(&app);
	if (app.timer)
		lv_timer_delete(app.timer);
	fplinux_ui_close(app.ui);
	fplinux_ui_prefs_close(app.prefs);
	return result < 0;
}
