/* SPDX-License-Identifier: 0BSD */
/*
 * LVGL widgets: what the hosting code (apidemo-lvgl.c) and the widget
 * screens (apidemo-lvgl-widgets.c) share.
 */
#ifndef FPLINUX_APIDEMO_LVGL_H
#define FPLINUX_APIDEMO_LVGL_H

#include "apidemo.h"

#define FOCUSABLE_LIMIT 12
#define BUFFER_LIMIT 12
/* Frames of the generated animation. */
#define FRAME_COUNT 8
#define CHART_POINTS 8
#define CHART_COUNT 2
#define BAR_COUNT 3
#define TABLE_ROWS 5
#define TABLE_COLUMNS 3
#define CALENDAR_ROWS 7
#define KEYBOARD_ROWS 5
#define KEYPAD_ROWS 4
#define DAYS_IN_WEEK 7
#define MONTHS 12
#define LED_COUNT 3
#define ROLLER_COUNT 2

/* What OK does on the focused widget; the centre legend names it. */
enum ok_action {
	OK_NONE,
	OK_PRESS,
	OK_TOGGLE,
	/* Mark or Unmark, by the checked state. */
	OK_MARK,
	/* Open, or Select while a dropdown's list is open. */
	OK_OPEN,
	OK_SELECT,
	OK_DONE,
	OK_NEW_LINE,
	/* Pause or Play, for the animation. */
	OK_PLAY,
	OK_RESET,
};

/* The keys that the focused widget uses itself; the others scroll. */
enum {
	USES_LEFT_RIGHT = 1U << 0,
	USES_UP_DOWN = 1U << 1,
	/* Digits, * and #, and the text of an external keyboard. */
	USES_TEXT = 1U << 2,
};
#define USES_ARROWS (USES_LEFT_RIGHT | USES_UP_DOWN)

struct focusable {
	lv_obj_t *object;
	/* Shown from its top down with the widget, such as its caption. */
	lv_obj_t *top;
	enum ok_action ok;
	unsigned int uses;
};

/* Styles made from the theme and the metrics, shared by the widgets. */
struct styles {
	/* Captions and key notes: secondary text. */
	lv_style_t note;
	lv_style_t large;
	/* The focus ring of a control without a frame of its own. */
	lv_style_t ring;
	/* A framed control, such as a field, and its focused frame. */
	lv_style_t frame;
	lv_style_t frame_focused;
	/* A button or key, focused it fills with accent; and disabled. */
	lv_style_t key;
	lv_style_t selected;
	lv_style_t disabled;
	/* A cell of a matrix or a table. */
	lv_style_t cell;
	/* A row of a list or a menu. */
	lv_style_t row;
	/* Tracks, fills and knobs of bars, sliders, switches and arcs. */
	lv_style_t track;
	lv_style_t fill;
	lv_style_t knob;
	lv_style_t arc_track;
	lv_style_t arc_fill;
	/* A warning section of the scale, not a categorical chart colour. */
	lv_style_t scale_warning;
	/* A layer above the page: dropdown lists and message boxes. */
	lv_style_t popup;
	/* Alpha-only images in the text colour and in the focus colour. */
	lv_style_t glyph;
	lv_style_t glyph_focus;
};

struct lvgl_demo;

struct widget_demo {
	void (*build)(struct lvgl_demo *demo);
	/* A key press the screen handles before the widgets: true if used. */
	bool (*key)(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		    uint32_t lvgl_key);
	/* OK for a widget that does not act on LV_KEY_ENTER: true if used. */
	bool (*ok)(struct lvgl_demo *demo, lv_obj_t *focused);
	/* Back closes something on the screen first: true if it did. */
	bool (*back)(struct lvgl_demo *demo);
	/* Up and Down move the focus, as between list rows. */
	bool rows;
};

struct chart_view {
	lv_obj_t *chart;
	lv_chart_series_t *series;
	lv_chart_cursor_t *cursor;
	uint32_t point;
};

/* The state of the screen shown, by widget. */
union demo_state {
	struct {
		const void *frames[FRAME_COUNT];
		lv_obj_t *image;
		bool running;
	} animimg;
	struct {
		lv_obj_t *bars[BAR_COUNT];
		lv_obj_t *values[BAR_COUNT];
	} bar;
	struct {
		unsigned int presses;
	} button;
	struct {
		lv_obj_t *keys;
		char typed[16];
	} matrix;
	struct {
		lv_obj_t *calendar;
		lv_obj_t *title;
		lv_calendar_date_t chosen;
		const char *day_names[DAYS_IN_WEEK];
		char months[MONTHS * 24];
		char years[64];
	} calendar;
	struct {
		lv_obj_t *canvas;
		int32_t x;
		int32_t y;
	} canvas;
	struct {
		struct chart_view views[CHART_COUNT];
	} chart;
	struct {
		lv_obj_t *star;
		int32_t angle;
	} image;
	struct {
		unsigned int presses;
	} imagebutton;
	struct {
		lv_obj_t *textarea;
	} keyboard;
	struct {
		lv_point_precise_t zigzag[7];
		lv_point_precise_t dashed[2];
		lv_point_precise_t thick[2];
	} line;
	struct {
		lv_obj_t *menu;
	} menu;
	struct {
		lv_obj_t *box;
		lv_obj_t *opener;
	} msgbox;
	struct {
		char hours[24 * 3];
		lv_obj_t *rollers[ROLLER_COUNT];
		/* The option each roller took with OK. */
		uint32_t chosen[ROLLER_COUNT];
	} roller;
	struct {
		lv_obj_t *round;
		lv_obj_t *needle;
		int32_t value;
		int32_t needle_length;
	} scale;
	struct {
		lv_obj_t *range;
		bool start;
	} slider;
	struct {
		lv_obj_t *tabview;
	} tabview;
	struct {
		lv_obj_t *tileview;
		uint32_t column;
		uint32_t row;
	} tileview;
};

struct lvgl_demo {
	struct apidemo *app;
	const struct widget_demo *spec;
	struct fplinux_ui_screen *screen;
	/* fplinux_ui_canvas_object(), and the object Up and Down scroll. */
	lv_obj_t *area;
	lv_obj_t *page;
	/* The keypad device, its group, and the key it reports. */
	lv_indev_t *keypad;
	lv_group_t *group;
	uint32_t read_key;
	lv_indev_state_t read_state;
	/* The LVGL key the device reports pressed, or 0. */
	uint32_t held;
	/*
	 * The focused widget was scrolled out of view: it shows no focus and
	 * takes no keys until Up, Down or Next bring the focus back.
	 */
	bool unfocused;
	/* The open dropdown list was moved into the canvas object. */
	bool list_placed;
	/* One native popup at a time; its full-surface scrim is a sibling. */
	lv_obj_t *popup;
	lv_obj_t *scrim;
	/* The label of the OK command on the screen, or NULL for none. */
	const char *ok_shown;
	struct focusable focusables[FOCUSABLE_LIMIT];
	size_t focusable_count;
	struct styles styles;
	/* Images decoded or drawn for the screen, freed with it. */
	lv_draw_buf_t *buffers[BUFFER_LIMIT];
	size_t buffer_count;
	/* A line that reports what the widgets did. */
	lv_obj_t *status;
	/* Next also reaches the key note, whose arrows scroll the page. */
	lv_obj_t *key_note;
	union demo_state u;
};

/* Sizes */

static inline const struct fplinux_ui_metrics *
metrics(const struct lvgl_demo *demo)
{
	return demo->app->metrics;
}

static inline const struct fplinux_ui_theme *theme(const struct lvgl_demo *demo)
{
	return demo->app->theme;
}

/* The width inside the page: the surface less margin and gutter. */
static inline int32_t content_width(const struct lvgl_demo *demo)
{
	return fplinux_ui_width(demo->app->ui) - metrics(demo)->margin -
	       metrics(demo)->gutter;
}

/* The height of the canvas object below the title band. */
static inline int32_t area_height(const struct lvgl_demo *demo)
{
	return fplinux_ui_height(demo->app->ui) - metrics(demo)->header_height;
}

static inline int32_t line_height(const struct lvgl_demo *demo)
{
	return lv_font_get_line_height(theme(demo)->font);
}

/* The widget screens, by entry variant (apidemo-lvgl-widgets.c). */
enum widget {
	WIDGET_ANIMIMG,
	WIDGET_ARC,
	WIDGET_ARCLABEL,
	WIDGET_BAR,
	WIDGET_BUTTON,
	WIDGET_BUTTONMATRIX,
	WIDGET_CALENDAR,
	WIDGET_CALENDAR_LISTS,
	WIDGET_CANVAS,
	WIDGET_CHART,
	WIDGET_CHECKBOX,
	WIDGET_DROPDOWN,
	WIDGET_IMAGE,
	WIDGET_IMAGEBUTTON,
	WIDGET_KEYBOARD,
	WIDGET_LABEL,
	WIDGET_LED,
	WIDGET_LINE,
	WIDGET_LIST,
	WIDGET_MENU,
	WIDGET_MSGBOX,
	WIDGET_ROLLER,
	WIDGET_SCALE,
	WIDGET_SLIDER,
	WIDGET_SPAN,
	WIDGET_SPINBOX,
	WIDGET_SPINNER,
	WIDGET_SWITCH,
	WIDGET_TABLE,
	WIDGET_TABVIEW,
	WIDGET_TEXTAREA,
	WIDGET_TILEVIEW,
	WIDGET_WIN,
	WIDGET_COUNT,
};

extern const struct widget_demo lvgl_widget_demos[WIDGET_COUNT];

/* Hosting and helpers (apidemo-lvgl.c) */

/* Removes the default theme's styles from object and its children. */
void lvgl_untheme(lv_obj_t *object);
/* A button or key in part: framed, accent while focused. */
void lvgl_style_key(struct lvgl_demo *demo, lv_obj_t *object,
		    lv_style_selector_t part);
/* A framed control whose frame turns to the focus colour. */
void lvgl_style_frame(struct lvgl_demo *demo, lv_obj_t *object);
/* The focus ring of a control without a frame. */
void lvgl_style_ring(struct lvgl_demo *demo, lv_obj_t *object);
/* The list of a dropdown: a popup with the selected option in accent. */
void lvgl_style_dropdown_list(struct lvgl_demo *demo, lv_obj_t *dropdown);
/* Native popups share the runtime's full-surface static scrim policy. */
void lvgl_popup_show(struct lvgl_demo *demo, lv_obj_t *popup);

lv_obj_t *lvgl_box_new(lv_obj_t *parent);
lv_obj_t *lvgl_row_new(struct lvgl_demo *demo, lv_obj_t *parent);
lv_obj_t *lvgl_column_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  int32_t width);
lv_obj_t *lvgl_line_new(lv_obj_t *parent, const char *text,
			const lv_font_t *font);
lv_obj_t *lvgl_caption_new(struct lvgl_demo *demo, lv_obj_t *parent,
			   const char *text);
lv_obj_t *lvgl_paragraph_new(lv_obj_t *parent, const char *text);
lv_obj_t *lvgl_note_new(struct lvgl_demo *demo, lv_obj_t *parent,
			const char *text);
lv_obj_t *lvgl_status_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  const char *text);
lv_obj_t *lvgl_button_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  const char *text);
/* Makes a row of the page reach both edges, like a list row. */
void lvgl_full_width(struct lvgl_demo *demo, lv_obj_t *row);
/* Keeps buffer until the screen goes; NULL stays NULL. */
lv_draw_buf_t *lvgl_keep_buffer(struct lvgl_demo *demo, lv_draw_buf_t *buffer);

/* Adds an interactive widget to the group, in focus order. */
void lvgl_add_focusable(struct lvgl_demo *demo, lv_obj_t *object,
			enum ok_action ok, unsigned int uses);
void lvgl_clear_focusables(struct lvgl_demo *demo);
/* When object takes the focus, the page shows it from top, its caption. */
void lvgl_show_with(struct lvgl_demo *demo, lv_obj_t *object, lv_obj_t *top);
/* Relabels the OK command for the focused widget. */
void lvgl_update_ok(struct lvgl_demo *demo);
/* Scrolls the page so that a band or a row of an object is visible. */
void lvgl_show_band(struct lvgl_demo *demo, int32_t top, int32_t bottom);
void lvgl_show_row(struct lvgl_demo *demo, lv_obj_t *object, uint32_t row,
		   uint32_t rows);
/* Gives a key change, or a press, to the keypad device. */
void lvgl_keypad_feed(struct lvgl_demo *demo, uint32_t key, bool pressed);
void lvgl_keypad_press(struct lvgl_demo *demo, uint32_t key);

#endif
