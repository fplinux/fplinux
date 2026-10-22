/* SPDX-License-Identifier: 0BSD */
/*
 * Canvas: a screen the application draws itself, with its own keys.
 *
 * fplinux_ui_canvas_object() is the drawing area: below the title band, or
 * the whole surface without a title. The application lays it out from the
 * surface size and the metrics, colours it from the theme and redraws only
 * what changed. Commands still name and take the soft keys and OK; every
 * other key, press and release, goes to the key handler. A canvas may take
 * the whole display with fplinux_ui_set_fullscreen() and lays itself out
 * again on FPLINUX_UI_EVENT_RESIZE.
 */
#include "apidemo.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

/* The drawing area's height: below the title band, or the whole surface. */
static int32_t area_height(struct apidemo *app, bool titled)
{
	return fplinux_ui_height(app->ui) -
	       (titled ? app->metrics->header_height : 0);
}

/*
 * A text that may not fit: a label that wraps inside a container that
 * scrolls, with margin at the left and the gutter for the scroll indicator
 * at the right. Up and Down scroll it through fplinux_ui_scroll_key().
 */
static lv_obj_t *scrolling_label(struct apidemo *app, lv_obj_t *parent,
				 lv_obj_t **scroll)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_t *label;

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_left(box, app->metrics->margin, 0);
	lv_obj_set_style_pad_right(box, app->metrics->gutter, 0);
	lv_obj_set_style_pad_ver(box, app->metrics->spacing, 0);
	apidemo_style_scroll(app, box);
	label = lv_label_create(box);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, app->theme->font, 0);
	lv_obj_set_style_text_color(label, app->theme->foreground, 0);
	*scroll = box;
	return label;
}

/* The Linux name of a key code, or NULL. */
static const char *key_name(unsigned int code)
{
	static const struct {
		unsigned int code;
		const char *name;
	} names[] = {
		{ KEY_F13, "KEY_F13" },
		{ KEY_F14, "KEY_F14" },
		{ KEY_OK, "KEY_OK" },
		{ KEY_UP, "KEY_UP" },
		{ KEY_DOWN, "KEY_DOWN" },
		{ KEY_LEFT, "KEY_LEFT" },
		{ KEY_RIGHT, "KEY_RIGHT" },
		{ KEY_NUMERIC_0, "KEY_NUMERIC_0" },
		{ KEY_NUMERIC_1, "KEY_NUMERIC_1" },
		{ KEY_NUMERIC_2, "KEY_NUMERIC_2" },
		{ KEY_NUMERIC_3, "KEY_NUMERIC_3" },
		{ KEY_NUMERIC_4, "KEY_NUMERIC_4" },
		{ KEY_NUMERIC_5, "KEY_NUMERIC_5" },
		{ KEY_NUMERIC_6, "KEY_NUMERIC_6" },
		{ KEY_NUMERIC_7, "KEY_NUMERIC_7" },
		{ KEY_NUMERIC_8, "KEY_NUMERIC_8" },
		{ KEY_NUMERIC_9, "KEY_NUMERIC_9" },
		{ KEY_NUMERIC_STAR, "KEY_NUMERIC_STAR" },
		{ KEY_NUMERIC_POUND, "KEY_NUMERIC_POUND" },
		{ KEY_PICKUP_PHONE, "KEY_PICKUP_PHONE" },
		{ KEY_ESC, "KEY_ESC" },
		{ KEY_ENTER, "KEY_ENTER" },
		{ KEY_KPENTER, "KEY_KPENTER" },
		{ KEY_BACKSPACE, "KEY_BACKSPACE" },
		{ KEY_DELETE, "KEY_DELETE" },
		{ KEY_TAB, "KEY_TAB" },
		{ KEY_SPACE, "KEY_SPACE" },
		{ KEY_HOME, "KEY_HOME" },
		{ KEY_END, "KEY_END" },
		{ KEY_COMPOSE, "KEY_COMPOSE" },
		{ KEY_LEFTSHIFT, "KEY_LEFTSHIFT" },
		{ KEY_RIGHTSHIFT, "KEY_RIGHTSHIFT" },
		{ KEY_LEFTCTRL, "KEY_LEFTCTRL" },
		{ KEY_LEFTALT, "KEY_LEFTALT" },
	};

	for (size_t i = 0; i < APIDEMO_COUNT(names); i++)
		if (names[i].code == code)
			return names[i].name;
	return NULL;
}

/* Writes the key's name, or its number, to out. */
static void format_code(unsigned int code, char *out, size_t size)
{
	const char *name = key_name(code);

	if (name)
		snprintf(out, size, "%s", name);
	else
		snprintf(out, size, "code %u", code);
}

/* Key tester */

static struct {
	lv_obj_t *label;
	/* The time of the last press, for the time it was held. */
	uint64_t pressed_ms;
	unsigned int pressed_code;
} tester;

/*
 * The fields of struct fplinux_ui_key: the code's name; pressed, or how
 * long it was held by time_ms; the source with the text a keyboard key
 * types; and what fplinux_ui_is_softkey() says. The tester takes every
 * key, so nothing scrolls: four short lines fit the smallest surface.
 */
static void tester_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	char code[32];
	char state[64];
	char source[64];
	char text[256];
	const char *soft =
		fplinux_ui_is_softkey(key, true)  ? _("left soft key") :
		fplinux_ui_is_softkey(key, false) ? _("right soft key") :
						    _("not a soft key");

	(void)ui;
	(void)canvas;
	(void)context;
	if (!tester.label)
		return;
	format_code(key->code, code, sizeof(code));
	/* time_ms measures intervals between keys of the same source. */
	if (key->pressed) {
		tester.pressed_ms = key->time_ms;
		tester.pressed_code = key->code;
		snprintf(state, sizeof(state), "%s", _("pressed"));
	} else if (key->code == tester.pressed_code &&
		   key->time_ms >= tester.pressed_ms) {
		snprintf(
			state, sizeof(state), _("held %llu\u00a0ms"),
			(unsigned long long)(key->time_ms - tester.pressed_ms));
	} else {
		snprintf(state, sizeof(state), "%s", _("released"));
	}
	/*
	 * The text is filled for presses and releases of a keyboard key.
	 * Escape, Enter, Tab and Backspace type control characters, which no
	 * font draws: show those by their code point.
	 */
	if (key->source == FPLINUX_UI_KEY_KEYBOARD && key->text[0] &&
	    ((unsigned char)key->text[0] < 0x20 ||
	     (unsigned char)key->text[0] == 0x7f))
		snprintf(source, sizeof(source), "KEYBOARD U+%04X",
			 (unsigned int)(unsigned char)key->text[0]);
	else if (key->source == FPLINUX_UI_KEY_KEYBOARD && key->text[0])
		snprintf(source, sizeof(source), "KEYBOARD “%s”", key->text);
	else
		snprintf(source, sizeof(source), "%s",
			 key->source == FPLINUX_UI_KEY_KEYBOARD ? "KEYBOARD" :
								  "PHONE");
	snprintf(text, sizeof(text), "%s\n%s\n%s\n%s", code, state, source,
		 soft);
	lv_label_set_text(tester.label, text);
}

static void tester_deleted(lv_event_t *event)
{
	(void)event;
	tester.label = NULL;
}

/*
 * variant 1 turns on fplinux_ui_canvas_set_keyboard_keys(): an external
 * keyboard's Escape and Enter come to the handler instead of the right
 * soft key and OK. The phone's keys keep the commands either way.
 */
static void open_tester(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	lv_obj_t *scroll;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	tester.label =
		scrolling_label(app, fplinux_ui_canvas_object(canvas), &scroll);
	tester.pressed_code = 0;
	lv_obj_add_event_cb(scroll, tester_deleted, LV_EVENT_DELETE, NULL);
	lv_label_set_text(tester.label,
			  entry->variant ?
				  _("Press a key. Escape and Enter of a "
				    "keyboard come here too.") :
				  _("Press a key. Only Back has a command."));
	/* Back takes the right soft key; the left one and OK come here. */
	apidemo_add_back(canvas);
	fplinux_ui_screen_set_listener(canvas, apidemo_back_listener, NULL,
				       app);
	fplinux_ui_canvas_set_key_handler(canvas, tester_key, NULL);
	fplinux_ui_canvas_set_keyboard_keys(canvas, entry->variant);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

/* The whole surface */

static struct {
	struct apidemo *app;
	lv_obj_t *label;
	lv_obj_t *scroll;
} whole;

static void show_whole(const char *last)
{
	char text[512];

	/* TRANSLATORS: %1$d × %2$d is the surface in pixels, %3$s a key. */
	snprintf(
		text, sizeof(text),
		_("Last key: %3$s\n\nA canvas without a title draws on the "
		  "whole surface, %1$d × %2$d\u00a0px. With no commands, every "
		  "key comes to its handler, which goes back on the right "
		  "soft key."),
		(int)fplinux_ui_width(whole.app->ui),
		(int)fplinux_ui_height(whole.app->ui), last);
	lv_label_set_text(whole.label, text);
}

/* No command takes a key here: the handler decides about every one. */
static void whole_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		      const struct fplinux_ui_key *key, void *context)
{
	char code[32];

	(void)canvas;
	(void)context;
	if (!key->pressed || !whole.label)
		return;
	if (fplinux_ui_is_softkey(key, false) ||
	    (key->source == FPLINUX_UI_KEY_KEYBOARD && key->code == KEY_ESC)) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (fplinux_ui_scroll_key(whole.scroll, key))
		return;
	format_code(key->code, code, sizeof(code));
	show_whole(code);
}

static void whole_deleted(lv_event_t *event)
{
	(void)event;
	whole.label = NULL;
	whole.scroll = NULL;
}

static void open_whole(struct apidemo *app, const struct apidemo_entry *entry)
{
	/* NULL: no title band. */
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(app->ui, NULL);
	lv_obj_t *area;
	lv_obj_t *frame;

	(void)entry;
	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	area = fplinux_ui_canvas_object(canvas);
	/* A frame along the edges of the drawing area. */
	frame = lv_obj_create(area);
	lv_obj_remove_style_all(frame);
	lv_obj_set_size(frame, fplinux_ui_width(app->ui),
			area_height(app, false));
	lv_obj_set_style_border_color(frame, app->theme->focus, 0);
	lv_obj_set_style_border_width(frame, app->metrics->focus_width, 0);
	lv_obj_set_style_pad_all(frame, app->metrics->focus_width, 0);
	lv_obj_set_scrollable(frame, false);
	whole.app = app;
	whole.label = scrolling_label(app, frame, &whole.scroll);
	lv_obj_add_event_cb(frame, whole_deleted, LV_EVENT_DELETE, NULL);
	show_whole(_("none yet"));
	fplinux_ui_canvas_set_key_handler(canvas, whole_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

/* LVGL objects */

/*
 * A square, an LVGL object, that the D-pad moves by its own size inside a
 * framed playground. Moving an object makes LVGL redraw its old and new
 * places only.
 */
static struct {
	lv_obj_t *square;
	int32_t step;
	int32_t columns;
	int32_t rows;
	int32_t x;
	int32_t y;
} mover;

static void mover_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		      const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (!key->pressed || !mover.square)
		return;
	switch (key->code) {
	case KEY_UP:
	case KEY_NUMERIC_2:
		mover.y -= mover.y > 0;
		break;
	case KEY_DOWN:
	case KEY_NUMERIC_8:
		mover.y += mover.y < mover.rows - 1;
		break;
	case KEY_LEFT:
	case KEY_NUMERIC_4:
		mover.x -= mover.x > 0;
		break;
	case KEY_RIGHT:
	case KEY_NUMERIC_6:
		mover.x += mover.x < mover.columns - 1;
		break;
	default:
		return;
	}
	lv_obj_set_pos(mover.square, mover.x * mover.step,
		       mover.y * mover.step);
}

static void mover_deleted(lv_event_t *event)
{
	(void)event;
	mover.square = NULL;
}

static void open_objects(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	const struct fplinux_ui_theme *theme = app->theme;
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	int32_t line = lv_font_get_line_height(theme->font);
	int32_t frame_width;
	int32_t frame_height;
	lv_obj_t *area;
	lv_obj_t *hint;
	lv_obj_t *field;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	area = fplinux_ui_canvas_object(canvas);
	hint = lv_label_create(area);
	lv_obj_set_pos(hint, metrics->margin, metrics->spacing);
	lv_obj_set_size(hint, fplinux_ui_width(app->ui) - 2 * metrics->margin,
			line);
	lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_font(hint, theme->font, 0);
	lv_obj_set_style_text_color(hint, theme->muted, 0);
	lv_label_set_text(hint, _("D-pad: move"));
	/* The playground: a border and a gap around the squares. */
	field = lv_obj_create(area);
	lv_obj_remove_style_all(field);
	lv_obj_set_style_border_color(field, theme->border, 0);
	lv_obj_set_style_border_width(field, metrics->focus_width, 0);
	lv_obj_set_style_pad_all(field, metrics->focus_width, 0);
	lv_obj_set_scrollable(field, false);
	/* LVGL places children inside both the border and the padding. */
	frame_width = lv_obj_get_style_border_width(field, LV_PART_MAIN) * 2 +
		      lv_obj_get_style_pad_left(field, LV_PART_MAIN) +
		      lv_obj_get_style_pad_right(field, LV_PART_MAIN);
	frame_height = lv_obj_get_style_border_width(field, LV_PART_MAIN) * 2 +
		       lv_obj_get_style_pad_top(field, LV_PART_MAIN) +
		       lv_obj_get_style_pad_bottom(field, LV_PART_MAIN);
	/* It fills the rest, a whole number of steps inside. */
	mover.step = metrics->row_icon_size;
	mover.columns = (fplinux_ui_width(app->ui) - 2 * metrics->margin -
			 frame_width) /
			mover.step;
	mover.rows = (area_height(app, true) - line - 3 * metrics->spacing -
		      frame_height) /
		     mover.step;
	if (mover.columns < 1)
		mover.columns = 1;
	if (mover.rows < 1)
		mover.rows = 1;
	lv_obj_set_size(field, mover.columns * mover.step + frame_width,
			mover.rows * mover.step + frame_height);
	lv_obj_set_pos(field, metrics->margin, line + 2 * metrics->spacing);
	mover.square = lv_obj_create(field);
	lv_obj_remove_style_all(mover.square);
	lv_obj_set_size(mover.square, mover.step, mover.step);
	lv_obj_set_style_bg_color(mover.square, theme->focus, 0);
	lv_obj_set_style_bg_opa(mover.square, LV_OPA_COVER, 0);
	mover.x = mover.columns / 2;
	mover.y = mover.rows / 2;
	lv_obj_set_pos(mover.square, mover.x * mover.step,
		       mover.y * mover.step);
	lv_obj_add_event_cb(field, mover_deleted, LV_EVENT_DELETE, NULL);
	apidemo_add_back(canvas);
	fplinux_ui_screen_set_listener(canvas, apidemo_back_listener, NULL,
				       app);
	fplinux_ui_canvas_set_key_handler(canvas, mover_key, NULL);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

/* Sketch pad: drawing in LV_EVENT_DRAW_MAIN */

enum {
	COMMAND_PAINT = 1,
	COMMAND_CLEAR,
	/* The display mode demo's switch and its content sizes. */
	COMMAND_MODE,
	COMMAND_SIZES,
};

#define SKETCH_CELLS 4096

static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *canvas;
	lv_obj_t *board;
	int32_t cell;
	int32_t columns;
	int32_t rows;
	int32_t x;
	int32_t y;
	/* The OK legend now: Paint or Erase. */
	bool erase;
	bool painted[SKETCH_CELLS];
} sketch;

/* A cell's rectangle in display coordinates, as drawing wants them. */
static void cell_area(int32_t x, int32_t y, lv_area_t *area)
{
	lv_area_t board;

	lv_obj_get_coords(sketch.board, &board);
	area->x1 = board.x1 + x * sketch.cell;
	area->y1 = board.y1 + y * sketch.cell;
	area->x2 = area->x1 + sketch.cell - 1;
	area->y2 = area->y1 + sketch.cell - 1;
}

/*
 * The board's own background is drawn first; this adds the painted cells
 * in the foreground colour and the cursor as a frame in the focus colour,
 * a shape as well as a colour. LVGL calls it only for invalidated areas.
 */
static void draw_sketch(lv_event_t *event)
{
	const struct fplinux_ui_theme *theme = sketch.app->theme;
	lv_layer_t *layer = lv_event_get_layer(event);
	lv_draw_rect_dsc_t rectangle;
	lv_area_t area;

	lv_draw_rect_dsc_init(&rectangle);
	rectangle.bg_color = theme->foreground;
	rectangle.bg_opa = LV_OPA_COVER;
	for (int32_t y = 0; y < sketch.rows; y++)
		for (int32_t x = 0; x < sketch.columns; x++) {
			if (!sketch.painted[y * sketch.columns + x])
				continue;
			cell_area(x, y, &area);
			lv_draw_rect(layer, &rectangle, &area);
		}
	lv_draw_rect_dsc_init(&rectangle);
	rectangle.bg_opa = LV_OPA_TRANSP;
	rectangle.border_color = theme->focus;
	rectangle.border_width = sketch.app->metrics->focus_width;
	rectangle.border_opa = LV_OPA_COVER;
	cell_area(sketch.x, sketch.y, &area);
	lv_draw_rect(layer, &rectangle, &area);
}

/* Redraw one cell, not the board. */
static void invalidate_cell(int32_t x, int32_t y)
{
	lv_area_t area;

	cell_area(x, y, &area);
	lv_obj_invalidate_area(sketch.board, &area);
}

/* OK names what it will do to the cell under the cursor. */
static void label_paint(void)
{
	bool erase = sketch.painted[sketch.y * sketch.columns + sketch.x];
	const struct fplinux_ui_command paint = {
		.id = COMMAND_PAINT,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = erase ? _("Erase") : _("Paint"),
	};

	if (erase == sketch.erase)
		return;
	sketch.erase = erase;
	fplinux_ui_screen_add_command(sketch.canvas, &paint);
}

static void sketch_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *canvas, int command_id,
			   void *context)
{
	bool *cell = &sketch.painted[sketch.y * sketch.columns + sketch.x];

	(void)canvas;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_PAINT) {
		*cell = !*cell;
		invalidate_cell(sketch.x, sketch.y);
		label_paint();
	} else if (command_id == COMMAND_CLEAR) {
		memset(sketch.painted, 0, sizeof(sketch.painted));
		lv_obj_invalidate(sketch.board);
		label_paint();
	}
}

static void sketch_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	int32_t x = sketch.x;
	int32_t y = sketch.y;

	(void)ui;
	(void)canvas;
	(void)context;
	if (!key->pressed || !sketch.board)
		return;
	switch (key->code) {
	case KEY_UP:
	case KEY_NUMERIC_2:
		y -= y > 0;
		break;
	case KEY_DOWN:
	case KEY_NUMERIC_8:
		y += y < sketch.rows - 1;
		break;
	case KEY_LEFT:
	case KEY_NUMERIC_4:
		x -= x > 0;
		break;
	case KEY_RIGHT:
	case KEY_NUMERIC_6:
		x += x < sketch.columns - 1;
		break;
	default:
		return;
	}
	if (x == sketch.x && y == sketch.y)
		return;
	invalidate_cell(sketch.x, sketch.y);
	sketch.x = x;
	sketch.y = y;
	invalidate_cell(x, y);
	label_paint();
}

static void sketch_deleted(lv_event_t *event)
{
	(void)event;
	sketch.board = NULL;
	sketch.canvas = NULL;
}

static void open_sketch(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_metrics *metrics = app->metrics;
	const struct fplinux_ui_command clear = {
		.id = COMMAND_CLEAR,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Clear all"),
	};
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	int32_t width;
	int32_t height;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	memset(&sketch, 0, sizeof(sketch));
	sketch.app = app;
	sketch.canvas = canvas;
	/* Half an icon box: about as many cells on every profile. */
	sketch.cell = metrics->row_icon_size / 2;
	width = fplinux_ui_width(app->ui) - 2 * metrics->margin;
	height = area_height(app, true) - 2 * metrics->spacing;
	sketch.columns = width / sketch.cell;
	sketch.rows = height / sketch.cell;
	while (sketch.columns * sketch.rows > SKETCH_CELLS)
		sketch.rows--;
	sketch.x = sketch.columns / 2;
	sketch.y = sketch.rows / 2;
	sketch.board = lv_obj_create(fplinux_ui_canvas_object(canvas));
	lv_obj_remove_style_all(sketch.board);
	lv_obj_set_size(sketch.board, sketch.columns * sketch.cell,
			sketch.rows * sketch.cell);
	lv_obj_align(sketch.board, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(sketch.board, app->theme->surface, 0);
	lv_obj_set_style_bg_opa(sketch.board, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(sketch.board, false);
	lv_obj_add_event_cb(sketch.board, draw_sketch, LV_EVENT_DRAW_MAIN,
			    NULL);
	lv_obj_add_event_cb(sketch.board, sketch_deleted, LV_EVENT_DELETE,
			    NULL);
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &clear);
	/* erase starts true: the first call adds Paint. */
	sketch.erase = true;
	label_paint();
	fplinux_ui_screen_set_listener(canvas, sketch_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(canvas, sketch_key, NULL);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

/* Scrolling */

static struct {
	lv_obj_t *scroll;
} reader;

/* Up and Down by a line, Left and Right by a page; true at either end. */
static void reader_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (reader.scroll)
		fplinux_ui_scroll_key(reader.scroll, key);
}

static void reader_deleted(lv_event_t *event)
{
	(void)event;
	reader.scroll = NULL;
}

static void open_reader(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(app->ui, _(entry->title));
	lv_obj_t *label;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	label = scrolling_label(app, fplinux_ui_canvas_object(canvas),
				&reader.scroll);
	lv_obj_add_event_cb(reader.scroll, reader_deleted, LV_EVENT_DELETE,
			    NULL);
	lv_label_set_text(
		label,
		_("The screens scroll by themselves. On a canvas, give each "
		  "key to fplinux_ui_scroll_key() with the object that holds "
		  "more than it shows.\n\n"
		  "Up and Down scroll by one line of the object's font, Left "
		  "and Right by a page. The function returns true when it used "
		  "the key, also at either end, so that the application does "
		  "not move anything else while the user reads.\n\n"
		  "It returns false for other keys, for keys with Ctrl or Alt "
		  "and when the content fits.\n\n"
		  "The scroll indicator at the right is a thumb in the focus "
		  "colour, without a track, as in every list.\n\n"
		  "This is the end of the text."));
	apidemo_add_back(canvas);
	fplinux_ui_screen_set_listener(canvas, apidemo_back_listener, NULL,
				       app);
	fplinux_ui_canvas_set_key_handler(canvas, reader_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

/* Display mode */

/*
 * fplinux_ui_set_fullscreen() switches between the surface between the
 * system bars and the whole display. The switch is asynchronous: the
 * manager resizes the surface, the runtime lays out its screens again and
 * the application hears FPLINUX_UI_EVENT_RESIZE (mode_resize()), then
 * lays out the canvas from fplinux_ui_screen_get_content_size(). On the
 * whole display nothing draws the legends, as on a MIDP full-screen
 * Canvas, so the canvas draws its own soft-key bar there.
 */
static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *canvas;
	lv_obj_t *scroll;
	lv_obj_t *label;
	/* The canvas's own soft-key bar, shown on the whole display. */
	lv_obj_t *bar;
	/* The mode that the switch's legend was made for. */
	bool fullscreen;
} mode;

/* The switch names the other mode. */
static void label_switch(bool fullscreen)
{
	const struct fplinux_ui_command command = {
		.id = COMMAND_MODE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = fullscreen ? _("Normal view") : _("Full screen"),
	};

	mode.fullscreen = fullscreen;
	fplinux_ui_screen_add_command(mode.canvas, &command);
}

/* The readout and the bar for the current size and mode. */
static void layout_mode(void)
{
	struct apidemo *app = mode.app;
	int32_t bar = app->metrics->softkey_height;
	bool fullscreen = fplinux_ui_is_fullscreen(app->ui);
	int width;
	int height;
	char text[1024];

	/* The canvas object's size, as the runtime lays it out. */
	if (!fplinux_ui_screen_get_content_size(mode.canvas, &width, &height)) {
		apidemo_error(app, "fplinux_ui_screen_get_content_size");
		return;
	}
	lv_obj_set_size(mode.scroll, width, fullscreen ? height - bar : height);
	lv_obj_set_hidden(mode.bar, !fullscreen);
	if (!fplinux_ui_softkey_bar_set(app->ui, mode.bar, _("Options"), "",
					_("Back")))
		perror("fplinux_ui_softkey_bar_set");
	if (fullscreen != mode.fullscreen)
		label_switch(fullscreen);
	/* TRANSLATORS: %1$d × %2$d is the surface and %4$d × %5$d the
	 * content area in pixels, %3$s "Yes" or "No", %6$s a paragraph. */
	snprintf(text, sizeof(text),
		 _("Surface: %1$d × %2$d\u00a0px\nFull screen: %3$s\n"
		   "Content area: %4$d × %5$d\u00a0px\n\n%6$s"),
		 fplinux_ui_width(app->ui), fplinux_ui_height(app->ui),
		 fullscreen ? _("Yes") : _("No"), width, height,
		 fullscreen ? _("No status or soft-key bar here, so the "
				"canvas draws its own. Options > Normal view "
				"brings the system bars back, and Back leaves "
				"the demo between them.") :
			      _("Options > Full screen asks for the whole "
				"display: the surface grows over the system "
				"bars and FPLINUX_UI_EVENT_RESIZE brings its "
				"new size. Options > Content sizes measures "
				"the other kinds of screens."));
	lv_label_set_text(mode.label, text);
}

/*
 * fplinux_ui_screen_get_content_size() of the other kinds at the current
 * size: a list, a form and a text box with a title band, an alert's box,
 * and EINVAL for a NULL screen. A screen leaves only through the stack, so
 * each measured one is pushed and popped at once, before anything is
 * drawn; the alert measured last shows the results.
 */
static void show_sizes(struct apidemo *app)
{
	struct fplinux_ui_screen *screens[] = {
		fplinux_ui_list_new(app->ui, _("List"),
				    FPLINUX_UI_LIST_IMPLICIT),
		fplinux_ui_form_new(app->ui, _("Form")),
		fplinux_ui_text_box_new(app->ui, _("Text box"), "", 16),
		fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO,
				     _("Content sizes"), NULL, 0),
	};
	int sizes[APIDEMO_COUNT(screens)][2] = { { 0 } };
	bool measured = true;
	char text[512];
	int unused;

	for (size_t i = 0; i < APIDEMO_COUNT(screens); i++) {
		measured = measured && screens[i] &&
			   fplinux_ui_screen_get_content_size(
				   screens[i], &sizes[i][0], &sizes[i][1]);
		if (screens[i] && i + 1 < APIDEMO_COUNT(screens)) {
			fplinux_ui_push(app->ui, screens[i]);
			fplinux_ui_pop(app->ui);
		}
	}
	if (!measured) {
		if (screens[3]) {
			fplinux_ui_push(app->ui, screens[3]);
			fplinux_ui_pop(app->ui);
		}
		apidemo_error(app, "fplinux_ui_screen_get_content_size");
		return;
	}
	errno = 0;
	fplinux_ui_screen_get_content_size(NULL, &unused, &unused);
	/* TRANSLATORS: widths × heights in pixels; %9$s an errno name. */
	snprintf(text, sizeof(text),
		 _("List: %1$d × %2$d\u00a0px\nForm: %3$d × %4$d\u00a0px\n"
		   "Text box: %5$d × %6$d\u00a0px\nAlert: %7$d × %8$d\u00a0px\n"
		   "NULL screen: %9$s"),
		 sizes[0][0], sizes[0][1], sizes[1][0], sizes[1][1],
		 sizes[2][0], sizes[2][1], sizes[3][0], sizes[3][1],
		 apidemo_errno_name(errno));
	fplinux_ui_alert_set_text(screens[3], text);
	fplinux_ui_push(app->ui, screens[3]);
}

static void mode_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *canvas, int command_id,
			 void *context)
{
	(void)canvas;
	if (command_id == COMMAND_SIZES) {
		show_sizes(context);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		/* The screens below belong between the bars. */
		if (!fplinux_ui_set_fullscreen(ui, false))
			perror("fplinux_ui_set_fullscreen");
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_MODE &&
		   !fplinux_ui_set_fullscreen(ui,
					      !fplinux_ui_is_fullscreen(ui))) {
		apidemo_error(context, "fplinux_ui_set_fullscreen");
	}
}

/* The readout may not fit a small surface: Up and Down scroll it. */
static void mode_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		     const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	(void)context;
	if (mode.scroll)
		fplinux_ui_scroll_key(mode.scroll, key);
}

static void mode_deleted(lv_event_t *event)
{
	(void)event;
	mode.canvas = NULL;
	mode.scroll = NULL;
	mode.label = NULL;
	mode.bar = NULL;
}

/* The category's FPLINUX_UI_EVENT_RESIZE hook. */
static void mode_resize(struct apidemo *app)
{
	(void)app;
	if (mode.canvas)
		layout_mode();
}

/* variant 1 has no title band: the content area is the whole surface. */
static void open_mode(struct apidemo *app, const struct apidemo_entry *entry)
{
	/* Second in Options, after the switch that layout_mode() adds. */
	const struct fplinux_ui_command sizes_command = {
		.id = COMMAND_SIZES,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Content sizes"),
		.priority = 1,
	};
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(
		app->ui, entry->variant ? NULL : _(entry->title));
	lv_obj_t *area;

	if (!canvas) {
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	area = fplinux_ui_canvas_object(canvas);
	mode.app = app;
	mode.canvas = canvas;
	mode.label = scrolling_label(app, area, &mode.scroll);
	lv_obj_add_event_cb(mode.scroll, mode_deleted, LV_EVENT_DELETE, NULL);
	mode.bar = fplinux_ui_softkey_bar_create(app->ui, area);
	if (!mode.bar) {
		int saved = errno;

		fplinux_ui_push(app->ui, canvas);
		fplinux_ui_pop(app->ui);
		errno = saved;
		apidemo_error(app, "fplinux_ui_softkey_bar_create");
		return;
	}
	apidemo_add_back(canvas);
	fplinux_ui_screen_add_command(canvas, &sizes_command);
	/* The opposite of the current mode: layout_mode() adds the switch. */
	mode.fullscreen = !fplinux_ui_is_fullscreen(app->ui);
	layout_mode();
	fplinux_ui_screen_set_listener(canvas, mode_command, NULL, app);
	fplinux_ui_canvas_set_key_handler(canvas, mode_key, NULL);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
}

static const struct apidemo_entry entries[] = {
	{ N_("Key tester"),
	  "fplinux_ui_canvas_set_key_handler · fplinux_ui_is_softkey",
	  open_tester, 0 },
	{ N_("Escape and Enter"),
	  "fplinux_ui_canvas_set_keyboard_keys(canvas, true)", open_tester, 1 },
	{ N_("Whole surface"), "fplinux_ui_canvas_new(ui, NULL) · no commands",
	  open_whole, 0 },
	{ N_("LVGL objects"), "fplinux_ui_canvas_object · lv_obj_set_pos",
	  open_objects, 0 },
	{ N_("Sketch pad"), "LV_EVENT_DRAW_MAIN · lv_obj_invalidate_area",
	  open_sketch, 0 },
	{ N_("Scrolling text"), "fplinux_ui_scroll_key", open_reader, 0 },
	{ N_("Full screen"),
	  "fplinux_ui_set_fullscreen · FPLINUX_UI_EVENT_RESIZE · "
	  "fplinux_ui_screen_get_content_size",
	  open_mode, 0 },
	{ N_("Full screen without a title"),
	  "fplinux_ui_canvas_new(ui, NULL) · fplinux_ui_is_fullscreen",
	  open_mode, 1 },
};

const struct apidemo_category apidemo_canvas = {
	.title = N_("Canvas"),
	.icon = APIDEMO_IMAGE_DIR "canvas-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.resize = mode_resize,
};
