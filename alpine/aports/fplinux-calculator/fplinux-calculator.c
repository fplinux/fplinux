/* SPDX-License-Identifier: GPL-2.0-only */
#include "calculator.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-locale.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define APP_ID "org.fplinux.calculator"
#define CLEAR_HOLD_MS 500U
#define _(text) gettext(text)

enum command {
	COMMAND_BACK,
	COMMAND_CLEAR,
	COMMAND_RESET,
	COMMAND_EQUALS,
	COMMAND_RETRY,
	COMMAND_STAY,
	COMMAND_DISCARD,
};

/* The D-pad as drawn: + up, × left, = OK, ÷ right, − down. */
static const struct operator_key {
	char operation;
	const char *glyph;
	int column;
	int row;
} operator_keys[] = {
	{ '+', "+", 1, 0 }, { '*', "×", 0, 1 }, { '=', "=", 1, 1 },
	{ '/', "÷", 2, 1 }, { '-', "−", 1, 2 },
};

#define OPERATOR_KEY_COUNT (sizeof(operator_keys) / sizeof(operator_keys[0]))

struct calculator_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_ui_screen *screen;
	struct fplinux_calculator calculator;
	char separator;
	lv_obj_t *pending;
	lv_obj_t *number;
	int number_top;
	int number_height;
	int number_width;
	const lv_font_t *number_font;
	lv_obj_t *cells[OPERATOR_KEY_COUNT];
	lv_obj_t *cell_labels[OPERATOR_KEY_COUNT];
	char highlighted;
	enum fplinux_calculator_state command_state;
	bool commands_ready;
	bool command_equals;
	bool digit_limit;
	lv_timer_t *hold_timer;
	uint64_t clear_pressed_ms;
	bool clear_held;
	bool failed;
	struct fplinux_ui_screen *save_alert;
	struct fplinux_storage_result saved;
	bool discard;
	bool finalized;
};

static void stop_hold(struct calculator_app *app)
{
	app->clear_held = false;
	if (app->hold_timer)
		lv_timer_pause(app->hold_timer);
}

static void fail(struct calculator_app *app, const char *operation)
{
	perror(operation);
	stop_hold(app);
	app->failed = true;
	fplinux_ui_quit(app->ui);
}

static bool save_state(struct calculator_app *app)
{
	char text[FPLINUX_CALCULATOR_SAVE_BYTES];
	struct fplinux_storage_result result;

	fplinux_calculator_save(&app->calculator, text, sizeof(text));
	if (!fplinux_ui_prefs_set(app->prefs, "state", text)) {
		result.state = FPLINUX_STORAGE_UNPUBLISHED;
		result.error = errno;
	} else {
		result = fplinux_ui_prefs_save(app->prefs);
	}
	if (result.state != FPLINUX_STORAGE_DURABLE &&
	    (app->saved.state != result.state ||
	     app->saved.error != result.error)) {
		errno = result.error;
		perror("fplinux-calculator: save state");
	} else if (result.state == FPLINUX_STORAGE_DURABLE &&
		   app->saved.error) {
		fputs("fplinux-calculator: calculation saved\n", stderr);
	}
	app->saved = result;
	if (app->screen)
		fplinux_ui_screen_set_title(
			app->screen, result.state == FPLINUX_STORAGE_DURABLE ?
					     _("Calculator") :
					     _("Calculator (unsaved)"));
	return result.state == FPLINUX_STORAGE_DURABLE;
}

static bool show_save_error(struct calculator_app *app);

static void save_error_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *screen, int command_id,
			       void *context)
{
	struct calculator_app *app = context;

	(void)screen;
	app->save_alert = NULL;
	if (command_id == COMMAND_RETRY) {
		if (!save_state(app)) {
			if (!show_save_error(app)) {
				fplinux_ui_close_reply(ui,
						       FPLINUX_UI_CLOSE_CANCEL);
			}
			return;
		}
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
	} else if (command_id == COMMAND_DISCARD) {
		app->discard = true;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
	} else {
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_CANCEL);
	}
}

static bool show_save_error(struct calculator_app *app)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_RETRY,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Retry"),
		  .icon = fplinux_ui_glyph(app->ui, "refresh",
					   FPLINUX_UI_GLYPH_ROW) },
		{ .id = COMMAND_STAY,
		  .type = FPLINUX_UI_COMMAND_BACK,
		  .label = _("Stay"),
		  .icon = fplinux_ui_glyph(app->ui, "cancel",
					   FPLINUX_UI_GLYPH_ROW) },
		{ .id = COMMAND_DISCARD,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = app->saved.state ==
					   FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				   _("Close anyway") :
				   _("Discard changes"),
		  .icon = fplinux_ui_glyph(app->ui, "delete",
					   FPLINUX_UI_GLYPH_ROW) },
	};
	struct fplinux_ui_screen *alert;

	stop_hold(app);
	if (app->save_alert && fplinux_ui_current(app->ui) == app->save_alert)
		fplinux_ui_pop(app->ui);
	app->save_alert = NULL;
	alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR,
		app->saved.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
			_("Saving not confirmed") :
			_("Calculation not saved"),
		fplinux_ui_storage_error(app->saved), 0);
	if (!alert)
		return false;
	for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
	     ++index) {
		if (fplinux_ui_screen_add_command(alert, &commands[index]))
			continue;
		fplinux_ui_push(app->ui, alert);
		fplinux_ui_pop(app->ui);
		return false;
	}
	fplinux_ui_screen_set_listener(alert, save_error_command, NULL, app);
	app->save_alert = alert;
	fplinux_ui_push(app->ui, alert);
	return true;
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct calculator_app *app = context;

	(void)ui;
	stop_hold(app);
	if (save_state(app))
		return FPLINUX_UI_CLOSE_READY;
	if (show_save_error(app))
		return FPLINUX_UI_CLOSE_WAIT;
	return FPLINUX_UI_CLOSE_CANCEL;
}

static const char *operator_glyph(char operation)
{
	for (size_t index = 0; index < OPERATOR_KEY_COUNT; ++index)
		if (operator_keys[index].operation == operation)
			return operator_keys[index].glyph;
	return "";
}

static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void highlight_operation(struct calculator_app *app, char operation)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);

	for (size_t index = 0; index < OPERATOR_KEY_COUNT; ++index) {
		bool selected = operator_keys[index].operation == operation;

		if (!app->cells[index])
			continue;
		lv_obj_set_style_bg_color(
			app->cells[index],
			selected ? theme->accent : theme->surface, 0);
		lv_obj_set_style_border_color(
			app->cells[index],
			selected ? theme->accent : theme->border, 0);
		lv_obj_set_style_text_color(
			app->cell_labels[index],
			selected ? theme->accent_text : theme->foreground, 0);
	}
	app->highlighted = operation;
}

/* While entering numbers, Clear leaves Back and a full Reset in Options. */
static bool sync_commands(struct calculator_app *app)
{
	enum fplinux_calculator_state state =
		fplinux_calculator_get_state(&app->calculator);
	bool equals = fplinux_calculator_can_evaluate(&app->calculator);
	bool resets = state == FPLINUX_CALCULATOR_RESULT ||
		      state == FPLINUX_CALCULATOR_ERROR;
	const struct fplinux_ui_command clear = {
		.id = COMMAND_CLEAR,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = resets ? _("Reset") : _("Clear"),
		.icon = fplinux_ui_glyph(app->ui,
					 resets ? "refresh" : "backspace",
					 FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command reset = {
		.id = COMMAND_RESET,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Reset"),
		.icon = fplinux_ui_glyph(app->ui, "refresh",
					 FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command evaluate = {
		.id = COMMAND_EQUALS,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Equals"),
		.icon = fplinux_ui_glyph(app->ui, "equal",
					 FPLINUX_UI_GLYPH_ROW),
	};

	if (app->commands_ready && app->command_state == state &&
	    app->command_equals == equals)
		return true;
	fplinux_ui_screen_remove_command(app->screen, COMMAND_CLEAR);
	fplinux_ui_screen_remove_command(app->screen, COMMAND_RESET);
	fplinux_ui_screen_remove_command(app->screen, COMMAND_EQUALS);
	if (state != FPLINUX_CALCULATOR_EMPTY &&
	    !fplinux_ui_screen_add_command(app->screen, &clear))
		return false;
	if ((state == FPLINUX_CALCULATOR_FIRST_NUMBER ||
	     state == FPLINUX_CALCULATOR_OPERATION) &&
	    !fplinux_ui_screen_add_command(app->screen, &reset))
		return false;
	if (equals && !fplinux_ui_screen_add_command(app->screen, &evaluate))
		return false;
	app->commands_ready = true;
	app->command_state = state;
	app->command_equals = equals;
	return true;
}

static int text_width(const char *text, const lv_font_t *font)
{
	lv_point_t size;

	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	return size.x;
}

/* All numeric characters, including exponent and minus, exist in font_clock. */
static void show_number(struct calculator_app *app, const char *text,
			const lv_font_t *font)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	int line;

	if (font == theme->font_clock &&
	    text_width(text, font) > app->number_width)
		font = theme->font_large;
	if (font == theme->font_large &&
	    text_width(text, font) > app->number_width)
		font = theme->font;
	if (font != app->number_font) {
		line = lv_font_get_line_height(font);
		lv_obj_set_style_text_font(app->number, font, 0);
		lv_obj_set_height(app->number, line);
		lv_obj_set_y(app->number,
			     app->number_top + app->number_height - line);
		app->number_font = font;
	}
	set_text(app->number, text);
}

static void refresh(struct calculator_app *app)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_calculator *calculator = &app->calculator;
	enum fplinux_calculator_state state =
		fplinux_calculator_get_state(calculator);
	bool error = state == FPLINUX_CALCULATOR_ERROR;
	char number[FPLINUX_CALCULATOR_TEXT_BYTES];
	char first[FPLINUX_CALCULATOR_TEXT_BYTES];
	char pending[128] = "";

	fplinux_calculator_display(calculator, app->separator, number,
				   sizeof(number));
	if (error) {
		snprintf(pending, sizeof(pending), "%s",
			 calculator->error ==
					 FPLINUX_CALCULATOR_DIVIDE_BY_ZERO ?
				 _("Divide by zero") :
				 _("Number too large"));
	} else if (app->digit_limit) {
		snprintf(pending, sizeof(pending), "%s",
			 _("Maximum 12 digits"));
	} else if (state == FPLINUX_CALCULATOR_OPERATION) {
		fplinux_calculator_format(calculator->accumulator,
					  app->separator, first, sizeof(first));
		snprintf(pending, sizeof(pending), "%s %s", first,
			 operator_glyph(calculator->operation));
	}
	set_text(app->pending, pending);
	show_number(app, error ? _("Error") : number,
		    error ? theme->font_large : theme->font_clock);
	if (calculator->operation != app->highlighted)
		highlight_operation(app, calculator->operation);
	if (!sync_commands(app))
		fail(app, "fplinux-calculator: commands");
}

static bool input(struct calculator_app *app, char key)
{
	enum fplinux_calculator_input_result result =
		fplinux_calculator_input(&app->calculator, key);

	if (result == FPLINUX_CALCULATOR_IGNORED)
		return false;
	app->digit_limit = result == FPLINUX_CALCULATOR_DIGIT_LIMIT;
	refresh(app);
	return result == FPLINUX_CALCULATOR_CHANGED;
}

static bool can_hold_clear(const struct calculator_app *app)
{
	return fplinux_ui_is_visible(app->ui) &&
	       fplinux_ui_lifecycle(app->ui) == FPLINUX_UI_FOREGROUND &&
	       fplinux_ui_current(app->ui) == app->screen &&
	       !fplinux_ui_menu_is_open(app->ui);
}

static void hold_elapsed(lv_timer_t *timer)
{
	struct calculator_app *app = lv_timer_get_user_data(timer);
	bool reset = app->clear_held && can_hold_clear(app);

	stop_hold(app);
	if (reset)
		input(app, 'C');
}

/* The filter owns Clear's release even when the initial delete empties it. */
static bool filter_key(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context)
{
	struct calculator_app *app = context;

	(void)ui;
	(void)screen;
	if (!fplinux_ui_is_softkey(key, false)) {
		if (key->pressed)
			stop_hold(app);
		return false;
	}
	if (!key->pressed) {
		bool held = app->clear_held;

		if (held && can_hold_clear(app) &&
		    key->time_ms >= app->clear_pressed_ms &&
		    key->time_ms - app->clear_pressed_ms >= CLEAR_HOLD_MS)
			input(app, 'C');
		stop_hold(app);
		return held;
	}
	if (fplinux_calculator_get_state(&app->calculator) ==
	    FPLINUX_CALCULATOR_EMPTY)
		return false;
	input(app, '<');
	app->clear_held = true;
	app->clear_pressed_ms = key->time_ms;
	lv_timer_reset(app->hold_timer);
	lv_timer_resume(app->hold_timer);
	return true;
}

static char calculator_key(const struct fplinux_ui_key *key)
{
	unsigned int code = key->code;
	char text = key->text[0];

	if (code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9)
		return (char)('0' + code - KEY_NUMERIC_0);
	switch (code) {
	case KEY_NUMERIC_STAR:
		return '.';
	case KEY_NUMERIC_POUND:
		return '#';
	case KEY_UP:
		return '+';
	case KEY_DOWN:
		return '-';
	case KEY_LEFT:
		return '*';
	case KEY_RIGHT:
		return '/';
	case KEY_OK:
	case KEY_ENTER:
	case KEY_KPENTER:
		return '=';
	case KEY_BACKSPACE:
		return '<';
	}
	if (key->source != FPLINUX_UI_KEY_KEYBOARD || !text || key->text[1])
		return '\0';
	if (text == 'x' || text == 'X')
		return '*';
	if (text == ',')
		return '.';
	return strchr("0123456789.+-*/=", text) ? text : '\0';
}

static void handle_key(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context)
{
	struct calculator_app *app = context;
	char value;

	(void)screen;
	if (!key->pressed)
		return;
	if (key->code == KEY_HOME)
		fplinux_ui_home(ui);
	else if ((value = calculator_key(key)))
		input(app, value);
}

static void handle_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	struct calculator_app *app = context;

	(void)screen;
	stop_hold(app);
	switch (command_id) {
	case COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_CLEAR:
		input(app, '<');
		break;
	case COMMAND_RESET:
		input(app, 'C');
		break;
	case COMMAND_EQUALS:
		input(app, '=');
		break;
	}
}

struct keypad {
	int left;
	int top;
	int cell_width;
	int cell_height;
	int gap;
};

static bool create_operator_keys(struct calculator_app *app,
				 const struct keypad *keypad)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	lv_obj_t *canvas = fplinux_ui_canvas_object(app->screen);

	for (size_t index = 0; index < OPERATOR_KEY_COUNT; ++index) {
		const struct operator_key *operator_key = &operator_keys[index];
		int x = keypad->left +
			operator_key->column *
				(keypad->cell_width + keypad->gap);
		int y = keypad->top +
			operator_key->row * (keypad->cell_height + keypad->gap);
		lv_obj_t *cell = lv_obj_create(canvas);
		lv_obj_t *label;

		if (!cell)
			return false;
		lv_obj_remove_style_all(cell);
		lv_obj_set_scrollable(cell, false);
		lv_obj_set_pos(cell, x, y);
		lv_obj_set_size(cell, keypad->cell_width, keypad->cell_height);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(cell, metrics->focus_width, 0);
		label = lv_label_create(cell);
		if (!label)
			return false;
		lv_obj_set_style_text_font(label, theme->font_large, 0);
		lv_label_set_text(label, operator_key->glyph);
		lv_obj_center(label);
		app->cells[index] = cell;
		app->cell_labels[index] = label;
	}
	highlight_operation(app, app->calculator.operation);
	return true;
}

static lv_obj_t *create_label(struct calculator_app *app, int x, int y,
			      int width, const char *text,
			      lv_text_align_t align)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	lv_obj_t *label =
		lv_label_create(fplinux_ui_canvas_object(app->screen));

	if (!label)
		return NULL;
	lv_obj_set_pos(label, x, y);
	lv_obj_set_size(label, width, lv_font_get_line_height(theme->font));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_font(label, theme->font, 0);
	lv_obj_set_style_text_color(label, theme->muted, 0);
	lv_obj_set_style_text_align(label, align, 0);
	lv_label_set_text(label, text);
	return label;
}

/* The Canvas owns only the calculation and its keypad, below the system title. */
static bool layout_view(struct calculator_app *app)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	lv_obj_t *canvas = fplinux_ui_canvas_object(app->screen);
	int width;
	int height;
	int content;
	int body = lv_font_get_line_height(theme->font);
	int title = lv_font_get_line_height(theme->font_large);
	int top = metrics->gap_small;
	int bottom;
	struct keypad keypad = { .gap = metrics->gap_small };
	int keys_space;
	int hint_y;
	int hint_width;
	char decimal_hint[8];

	stop_hold(app);
	if (!fplinux_ui_screen_get_content_size(app->screen, &width, &height))
		return false;
	lv_obj_clean(canvas);
	memset(app->cells, 0, sizeof(app->cells));
	memset(app->cell_labels, 0, sizeof(app->cell_labels));
	app->number_font = NULL;
	content = width - 2 * metrics->margin;
	bottom = height - metrics->gap_small;
	app->pending = create_label(app, metrics->margin, top, content, "",
				    LV_TEXT_ALIGN_RIGHT);
	app->number_top = top + body;
	app->number_height = lv_font_get_line_height(theme->font_clock);
	app->number_width = content;
	app->number = create_label(app, metrics->margin, app->number_top,
				   content, "", LV_TEXT_ALIGN_RIGHT);
	if (!app->pending || !app->number)
		return false;
	lv_obj_set_style_text_color(app->number, theme->foreground, 0);
	keypad.top = app->number_top + app->number_height + metrics->spacing;
	keys_space = bottom - keypad.top;
	keypad.cell_height = (keys_space - 2 * keypad.gap) / 3;
	snprintf(decimal_hint, sizeof(decimal_hint), "* %c", app->separator);
	if (keypad.cell_height >= title) {
		if (keypad.cell_height > title + 2 * metrics->spacing)
			keypad.cell_height = title + 2 * metrics->spacing;
		keypad.cell_width = (content - 2 * keypad.gap) / 3;
		if (keypad.cell_width > keypad.cell_height * 8 / 5)
			keypad.cell_width = keypad.cell_height * 8 / 5;
		keypad.left =
			(width - 3 * keypad.cell_width - 2 * keypad.gap) / 2;
		keypad.top +=
			(keys_space - 3 * keypad.cell_height - 2 * keypad.gap) /
			2;
		if (!create_operator_keys(app, &keypad))
			return false;
		hint_y = keypad.top + 2 * (keypad.cell_height + keypad.gap) +
			 (keypad.cell_height - body) / 2;
		hint_width = keypad.left + keypad.cell_width - metrics->margin;
	} else {
		hint_y = bottom - body;
		hint_width = content / 2;
		if (!create_label(app, metrics->margin, hint_y - body, content,
				  "↑+ ←× →÷ ↓−", LV_TEXT_ALIGN_LEFT))
			return false;
	}
	if (!create_label(app, metrics->margin, hint_y, hint_width,
			  decimal_hint, LV_TEXT_ALIGN_LEFT) ||
	    !create_label(app, width - metrics->margin - hint_width, hint_y,
			  hint_width, "# ±", LV_TEXT_ALIGN_RIGHT))
		return false;
	refresh(app);
	return !app->failed;
}

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct calculator_app *app = context;

	if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
	    event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		stop_hold(app);
		fplinux_ui_menu_close(ui);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		if (event->key.source == FPLINUX_UI_KEY_PHONE)
			stop_hold(app);
	} else if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		stop_hold(app);
		app->finalized = true;
		if (!app->discard)
			save_state(app);
	} else if ((event->type == FPLINUX_UI_EVENT_VISIBILITY &&
		    !event->visible) ||
		   (event->type == FPLINUX_UI_EVENT_LIFECYCLE &&
		    fplinux_ui_lifecycle(ui) != FPLINUX_UI_FOREGROUND)) {
		stop_hold(app);
		if (app->screen && !app->discard)
			save_state(app);
	} else if (event->type == FPLINUX_UI_EVENT_RESIZE && app->screen &&
		   !layout_view(app)) {
		fail(app, "fplinux-calculator: layout");
	}
}

static bool create_view(struct calculator_app *app)
{
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.priority = 10,
		.icon = fplinux_ui_glyph(app->ui, "back", FPLINUX_UI_GLYPH_ROW),
	};

	app->screen = fplinux_ui_canvas_new(app->ui, _("Calculator"));
	if (!app->screen)
		return false;
	lv_obj_set_scrollable(fplinux_ui_canvas_object(app->screen), false);
	app->hold_timer = lv_timer_create(hold_elapsed, CLEAR_HOLD_MS, app);
	if (!app->hold_timer)
		return false;
	lv_timer_pause(app->hold_timer);
	if (!fplinux_ui_screen_add_command(app->screen, &back))
		return false;
	fplinux_ui_screen_set_listener(app->screen, handle_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(app->screen, handle_key, app);
	fplinux_ui_screen_set_key_filter(app->screen, filter_key, app);
	if (!layout_view(app))
		return false;
	fplinux_ui_push(app->ui, app->screen);
	return true;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-calculator",
		.description =
			"Keypad calculator for the graphical phone session",
	};
	struct calculator_app app = {
		.saved = { .state = FPLINUX_STORAGE_DURABLE },
	};
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	app.prefs = fplinux_ui_prefs_open(APP_ID);
	if (!app.prefs) {
		perror("fplinux-calculator: preferences");
		return 1;
	}
	fplinux_calculator_restore(
		&app.calculator, fplinux_ui_prefs_get(app.prefs, "state", ""));
	app.ui = fplinux_ui_open(APP_ID, handle_event, &app);
	if (!app.ui) {
		perror("fplinux-calculator: open UI");
		fplinux_ui_prefs_close(app.prefs);
		return 1;
	}
	app.separator = fplinux_ui_decimal_separator(app.ui);
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	if (!create_view(&app)) {
		perror("fplinux-calculator: view");
		result = 1;
	} else {
		result = fplinux_ui_run(app.ui) < 0;
		stop_hold(&app);
		if (!app.finalized && !app.discard)
			save_state(&app);
	}
	if (app.hold_timer)
		lv_timer_delete(app.hold_timer);
	fplinux_ui_close(app.ui);
	fplinux_ui_prefs_close(app.prefs);
	return result || app.failed;
}
