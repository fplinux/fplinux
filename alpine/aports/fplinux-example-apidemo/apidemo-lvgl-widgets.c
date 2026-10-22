/* SPDX-License-Identifier: 0BSD */
/*
 * LVGL widgets: one screen per widget of the system LVGL. Each screen
 * builds its widgets on the page of apidemo-lvgl.c, which hosts it: the
 * keypad device, the focus, the OK and Next commands and the styles.
 */
#include "apidemo-lvgl.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * image_recolor is not inherited. A row or button's alpha-only glyph
 * children therefore take its text colour explicitly when focus changes.
 */
static void glyph_children_changed(lv_event_t *event)
{
	lv_obj_t *parent = lv_event_get_current_target_obj(event);
	lv_color_t colour = lv_obj_get_style_text_color(parent, LV_PART_MAIN);

	for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);

		if (lv_obj_check_type(child, &lv_image_class))
			lv_obj_set_style_image_recolor(child, colour, 0);
	}
}

static void style_glyph_children(struct lvgl_demo *demo, lv_obj_t *parent)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);

		if (lv_obj_check_type(child, &lv_image_class))
			lv_obj_add_style(child, &demo->styles.glyph, 0);
	}
	lv_obj_add_event_cb(parent, glyph_children_changed, LV_EVENT_FOCUSED,
			    NULL);
	lv_obj_add_event_cb(parent, glyph_children_changed, LV_EVENT_DEFOCUSED,
			    NULL);
}

/* Animated image */

/* A frame of the loader: eight dots on a circle, the step-th the darkest. */
static lv_draw_buf_t *dots_frame(int32_t size, int step)
{
	lv_draw_buf_t *frame =
		lv_draw_buf_create(size, size, LV_COLOR_FORMAT_A8, 0);
	int32_t centre = size / 2;
	int32_t orbit = size * 3 / 8;
	int32_t dot = size / 10 + 1;

	if (!frame)
		return NULL;
	lv_draw_buf_clear(frame, NULL);
	for (int i = 0; i < FRAME_COUNT; i++) {
		int32_t angle = i * 360 / FRAME_COUNT;
		int32_t x =
			centre + orbit * lv_trigo_cos(angle) / LV_TRIGO_SIN_MAX;
		int32_t y =
			centre + orbit * lv_trigo_sin(angle) / LV_TRIGO_SIN_MAX;
		/* The newest dot is opaque, the older ones fade. */
		int age = (step - i + FRAME_COUNT) % FRAME_COUNT;
		uint8_t alpha = (uint8_t)(LV_OPA_COVER * (FRAME_COUNT - age) /
					  FRAME_COUNT);

		for (int32_t py = y - dot; py <= y + dot; py++) {
			uint8_t *line;

			if (py < 0 || py >= size)
				continue;
			line = frame->data + py * frame->header.stride;
			for (int32_t px = x - dot; px <= x + dot; px++) {
				int32_t dx = px - x, dy = py - y;

				if (px >= 0 && px < size &&
				    dx * dx + dy * dy <= dot * dot &&
				    line[px] < alpha)
					line[px] = alpha;
			}
		}
	}
	return frame;
}

static void build_animimg(struct lvgl_demo *demo)
{
	int32_t size = metrics(demo)->row_icon_size * 2;
	lv_obj_t *image;

	for (int i = 0; i < FRAME_COUNT; i++) {
		lv_draw_buf_t *frame =
			lvgl_keep_buffer(demo, dots_frame(size, i));

		if (!frame) {
			apidemo_error(demo->app, "lv_draw_buf_create");
			return;
		}
		demo->u.animimg.frames[i] = frame;
	}
	image = lv_animimg_create(demo->page);
	lvgl_untheme(image);
	lv_obj_add_style(image, &demo->styles.glyph_focus, 0);
	lvgl_style_ring(demo, image);
	lv_animimg_set_src(image, demo->u.animimg.frames, FRAME_COUNT);
	lv_animimg_set_duration(image, 1000);
	lv_animimg_set_repeat_count(image, LV_ANIM_REPEAT_INFINITE);
	lv_animimg_start(image);
	demo->u.animimg.image = image;
	demo->u.animimg.running = true;
	lvgl_add_focusable(demo, image, OK_PLAY, 0);
	lvgl_note_new(
		demo, demo->page,
		_("Eight frames drawn in code, one every 125 ms. OK: pause or "
		  "play. The runtime stops every animation while the phone "
		  "is locked."));
}

static bool ok_animimg(struct lvgl_demo *demo, lv_obj_t *focused)
{
	(void)focused;
	if (demo->u.animimg.running)
		lv_animimg_delete(demo->u.animimg.image);
	else
		lv_animimg_start(demo->u.animimg.image);
	demo->u.animimg.running = !demo->u.animimg.running;
	return true;
}

/* Arc */

static void arc_changed(lv_event_t *event)
{
	lv_obj_t *arc = lv_event_get_target_obj(event);
	lv_obj_t *value = lv_event_get_user_data(event);

	lv_label_set_text_fmt(value, "%d", (int)lv_arc_get_value(arc));
}

static void arc_new(struct lvgl_demo *demo, lv_obj_t *parent,
		    const char *caption, lv_arc_mode_t mode, int32_t minimum,
		    int32_t maximum, int32_t value)
{
	int32_t size = (content_width(demo) - metrics(demo)->spacing) / 2;
	lv_obj_t *column = lvgl_column_new(demo, parent, size);
	lv_obj_t *arc;
	lv_obj_t *label;

	lvgl_caption_new(demo, column, caption);
	arc = lv_arc_create(column);
	lvgl_untheme(arc);
	lv_obj_set_size(arc, size, size);
	lv_obj_add_style(arc, &demo->styles.arc_track, LV_PART_MAIN);
	lv_obj_add_style(arc, &demo->styles.arc_fill, LV_PART_INDICATOR);
	lv_obj_add_style(arc, &demo->styles.knob, LV_PART_KNOB);
	lvgl_style_ring(demo, arc);
	lv_obj_set_style_radius(arc, LV_RADIUS_CIRCLE, 0);
	lv_arc_set_mode(arc, mode);
	lv_arc_set_range(arc, minimum, maximum);
	lv_arc_set_value(arc, value);
	label = lv_label_create(arc);
	lv_obj_add_style(label, &demo->styles.large, 0);
	lv_obj_center(label);
	lv_obj_add_event_cb(arc, arc_changed, LV_EVENT_VALUE_CHANGED, label);
	lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, NULL);
	/* The arc takes Up and Down too; here they move the focus. */
	lvgl_add_focusable(demo, arc, OK_NONE, USES_LEFT_RIGHT);
	lvgl_show_with(demo, arc, column);
}

static void build_arc(struct lvgl_demo *demo)
{
	lv_obj_t *row = lvgl_row_new(demo, demo->page);

	arc_new(demo, row, _("Normal"), LV_ARC_MODE_NORMAL, 0, 20, 8);
	arc_new(demo, row, _("Symmetrical"), LV_ARC_MODE_SYMMETRICAL, -10, 10,
		3);
	arc_new(demo, row, _("Reverse"), LV_ARC_MODE_REVERSE, 0, 20, 5);
	lvgl_note_new(demo, demo->page,
		      _("Left/Right: value. Next: the next arc."));
}

/* Arc label */

static void build_arclabel(struct lvgl_demo *demo)
{
	int32_t size = content_width(demo);
	lv_obj_t *label = lv_arclabel_create(demo->page);

	lvgl_untheme(label);
	/* The only widget of the screen: it needs no focus ring. */
	lv_obj_set_size(label, size, size);
	lv_arclabel_set_text(label, _("Text that follows an arc"));
	lv_arclabel_set_radius(label, size / 2 - line_height(demo));
	lv_arclabel_set_angle_start(label, 180);
	lv_arclabel_set_angle_size(label, 360);
	lv_arclabel_set_dir(label, LV_ARCLABEL_DIR_CLOCKWISE);
	lvgl_add_focusable(demo, label, OK_NONE, 0);
	lvgl_note_new(demo, demo->page, _("Left/Right: turn the text."));
}

static bool key_arclabel(struct lvgl_demo *demo,
			 const struct fplinux_ui_key *key, uint32_t code)
{
	lv_obj_t *label = demo->focusables[0].object;
	int32_t step = code == LV_KEY_LEFT ? -15 : 15;

	(void)key;
	if (code != LV_KEY_LEFT && code != LV_KEY_RIGHT)
		return false;
	lv_arclabel_set_angle_start(
		label, (lv_arclabel_get_angle_start(label) + step + 360) % 360);
	return true;
}

/* Bar */

/* A number as the language writes it: with a minus sign, not a hyphen. */
static void format_signed(char *out, size_t size, int32_t value)
{
	if (value < 0)
		snprintf(out, size, "−%d", (int)-value);
	else
		snprintf(out, size, "%d", (int)value);
}

/* A one-line "name: value" label; the name stays in its user data. */
static lv_obj_t *value_line_new(struct lvgl_demo *demo, const char *name)
{
	lv_obj_t *label = lvgl_line_new(demo->page, "", theme(demo)->font);

	lv_obj_set_user_data(label, (void *)name);
	return label;
}

static void value_line_set(lv_obj_t *label, const char *value)
{
	/* TRANSLATORS: a name and its value, such as "Range: 20–70". */
	lv_label_set_text_fmt(label, _("%s: %s"),
			      (const char *)lv_obj_get_user_data(label), value);
}

static void bar_show(struct lvgl_demo *demo, int index)
{
	lv_obj_t *bar = demo->u.bar.bars[index];
	char text[48], start[16], end[16];

	format_signed(start, sizeof(start), lv_bar_get_start_value(bar));
	format_signed(end, sizeof(end), lv_bar_get_value(bar));
	if (lv_bar_get_mode(bar) == LV_BAR_MODE_RANGE)
		snprintf(text, sizeof(text), "%s–%s", start, end);
	else
		/* TRANSLATORS: a percentage, such as 40%; a language that
		 * spaces it uses a no-break space (U+00A0). */
		snprintf(text, sizeof(text), _("%s%%"), end);
	value_line_set(demo->u.bar.values[index], text);
}

static void bar_new(struct lvgl_demo *demo, int index, const char *caption,
		    lv_bar_mode_t mode, int32_t minimum, int32_t maximum,
		    int32_t start, int32_t value)
{
	lv_obj_t *bar;

	demo->u.bar.values[index] = value_line_new(demo, caption);
	bar = lv_bar_create(demo->page);
	lvgl_untheme(bar);
	lv_obj_set_size(bar, lv_pct(100), metrics(demo)->spacing);
	lv_obj_add_style(bar, &demo->styles.track, LV_PART_MAIN);
	lv_obj_add_style(bar, &demo->styles.fill, LV_PART_INDICATOR);
	lvgl_style_ring(demo, bar);
	lv_bar_set_mode(bar, mode);
	lv_bar_set_range(bar, minimum, maximum);
	lv_bar_set_value(bar, value, LV_ANIM_OFF);
	if (mode == LV_BAR_MODE_RANGE)
		lv_bar_set_start_value(bar, start, LV_ANIM_OFF);
	demo->u.bar.bars[index] = bar;
	bar_show(demo, index);
	lvgl_add_focusable(demo, bar, OK_NONE, 0);
	lvgl_show_with(demo, bar, demo->u.bar.values[index]);
}

static void build_bar(struct lvgl_demo *demo)
{
	bar_new(demo, 0, _("Value"), LV_BAR_MODE_NORMAL, 0, 100, 0, 40);
	bar_new(demo, 1, _("Range"), LV_BAR_MODE_RANGE, 0, 100, 20, 70);
	bar_new(demo, 2, _("Symmetrical"), LV_BAR_MODE_SYMMETRICAL, -50, 50, 0,
		-20);
	lvgl_note_new(
		demo, demo->page,
		_("A bar takes no keys: Left/Right call lv_bar_set_value(). "
		  "Next: the next bar."));
}

static bool key_bar(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		    uint32_t code)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);
	int32_t step = code == LV_KEY_LEFT ? -10 : 10;

	(void)key;
	if (code != LV_KEY_LEFT && code != LV_KEY_RIGHT)
		return false;
	for (int i = 0; i < BAR_COUNT; i++) {
		lv_obj_t *bar = demo->u.bar.bars[i];
		int32_t value = lv_bar_get_value(bar) + step;

		if (bar != focused)
			continue;
		if (lv_bar_get_mode(bar) == LV_BAR_MODE_RANGE &&
		    value < lv_bar_get_start_value(bar))
			value = lv_bar_get_start_value(bar);
		lv_bar_set_value(bar, value, LV_ANIM_OFF);
		bar_show(demo, i);
	}
	return true;
}

/* Button */

static void button_clicked(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);

	demo->u.button.presses++;
	lv_label_set_text_fmt(demo->status, _("Presses: %u"),
			      demo->u.button.presses);
}

/* A checkable button says its state in words too: never colour alone. */
static void checkable_changed(lv_event_t *event)
{
	lv_obj_t *button = lv_event_get_target_obj(event);
	lv_obj_t *label = lv_obj_get_child(button, 0);

	lv_label_set_text(label, lv_obj_is_checked(button) ? _("Sound: on") :
							     _("Sound: off"));
}

static void build_button(struct lvgl_demo *demo)
{
	lv_obj_t *row = lvgl_row_new(demo, demo->page);
	lv_obj_t *button = lvgl_button_new(demo, row, _("Add one"));
	lv_obj_t *checkable = lvgl_button_new(demo, row, "");
	lv_obj_t *disabled = lvgl_button_new(demo, row, _("Unavailable"));

	lv_obj_add_event_cb(button, button_clicked, LV_EVENT_CLICKED, demo);
	lvgl_add_focusable(demo, button, OK_PRESS, 0);
	lv_obj_set_checkable(checkable, true);
	lv_obj_add_event_cb(checkable, checkable_changed,
			    LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_send_event(checkable, LV_EVENT_VALUE_CHANGED, NULL);
	lvgl_add_focusable(demo, checkable, OK_TOGGLE, 0);
	/* A disabled widget is drawn in disabled and takes no focus. */
	lv_obj_set_disabled(disabled, true);
	lvgl_status_new(demo, demo->page, _("Presses: 0"));
	lvgl_note_new(
		demo, demo->page,
		_("OK: press the focused button. Next: the next button. The "
		  "disabled button takes no focus."));
}

/* Button matrix */

static const char *const phone_keys[] = {
	"1", "2", "3", "\n", "4", "5", "6", "\n",
	"7", "8", "9", "\n", "*", "0", "#", "",
};

static void phone_key_typed(struct lvgl_demo *demo, const char *text)
{
	char *typed = demo->u.matrix.typed;
	size_t length = strlen(typed);

	/* Keep the last characters that fit the buffer. */
	if (length + 1 >= sizeof(demo->u.matrix.typed)) {
		memmove(typed, typed + 1, length);
		length--;
	}
	typed[length] = text[0];
	typed[length + 1] = '\0';
	lv_label_set_text_fmt(demo->status, _("Typed: %s"), typed);
}

static void phone_keys_changed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *matrix = lv_event_get_target_obj(event);
	uint32_t id = lv_buttonmatrix_get_selected_button(matrix);
	const char *text = lv_buttonmatrix_get_button_text(matrix, id);

	if (text)
		phone_key_typed(demo, text);
	lvgl_show_row(demo, matrix, id / 3, KEYPAD_ROWS);
}

static void segments_changed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *matrix = lv_event_get_target_obj(event);
	uint32_t id = lv_buttonmatrix_get_selected_button(matrix);

	lv_label_set_text_fmt(demo->status, _("Shown: %s"),
			      lv_buttonmatrix_get_button_text(matrix, id));
}

static lv_obj_t *matrix_new(struct lvgl_demo *demo, const char *const map[],
			    int32_t rows)
{
	lv_obj_t *matrix = lv_buttonmatrix_create(demo->page);

	lvgl_untheme(matrix);
	lv_buttonmatrix_set_map(matrix, map);
	lv_obj_set_size(
		matrix, lv_pct(100),
		rows * (line_height(demo) + metrics(demo)->gap_small * 2) +
			(rows - 1) * metrics(demo)->gap_small);
	lv_obj_set_style_pad_all(matrix, 0, 0);
	lv_obj_set_style_pad_gap(matrix, metrics(demo)->gap_small, 0);
	lv_obj_add_style(matrix, &demo->styles.cell, LV_PART_ITEMS);
	lv_obj_add_style(matrix, &demo->styles.selected,
			 LV_PART_ITEMS | LV_STATE_FOCUSED);
	lv_obj_add_style(matrix, &demo->styles.disabled,
			 LV_PART_ITEMS | LV_STATE_DISABLED);
	return matrix;
}

static void build_buttonmatrix(struct lvgl_demo *demo)
{
	/* The labels of the one-checked row, translated when shown. */
	static const char *segments[4];
	lv_obj_t *keypad;
	lv_obj_t *days;

	segments[0] = _("Day");
	segments[1] = _("Week");
	segments[2] = _("Month");
	segments[3] = "";

	lvgl_caption_new(demo, demo->page, _("Keys"));
	keypad = matrix_new(demo, phone_keys, KEYPAD_ROWS);
	lv_obj_add_event_cb(keypad, phone_keys_changed, LV_EVENT_VALUE_CHANGED,
			    demo);
	demo->u.matrix.keys = keypad;
	lvgl_add_focusable(demo, keypad, OK_PRESS, USES_ARROWS | USES_TEXT);
	lvgl_show_with(demo, keypad, lv_obj_get_sibling(keypad, -1));
	lvgl_status_new(demo, demo->page, _("Typed: "));

	lvgl_caption_new(demo, demo->page, _("One checked"));
	days = matrix_new(demo, segments, 1);
	lv_buttonmatrix_set_button_ctrl_all(days,
					    LV_BUTTONMATRIX_CTRL_CHECKABLE);
	lv_buttonmatrix_set_one_checked(days, true);
	lv_buttonmatrix_set_button_ctrl(days, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
	/* Checked is a shape too, an underline, not a colour alone. */
	lv_obj_set_style_border_side(days, LV_BORDER_SIDE_BOTTOM,
				     LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_border_width(days, metrics(demo)->focus_width * 2,
				      LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_border_color(days, theme(demo)->focus,
				      LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_add_event_cb(days, segments_changed, LV_EVENT_VALUE_CHANGED,
			    demo);
	lvgl_add_focusable(demo, days, OK_SELECT, USES_LEFT_RIGHT);
	lvgl_show_with(demo, days, lv_obj_get_sibling(days, -1));
	lvgl_note_new(demo, demo->page,
		      _("Arrows: move between keys. OK: press. Digits, * and # "
			"press their keys. Next: the other matrix."));
}

/* The phone's digit keys press the matching key of the keypad matrix. */
static bool key_buttonmatrix(struct lvgl_demo *demo,
			     const struct fplinux_ui_key *key, uint32_t code)
{
	lv_obj_t *keypad = demo->u.matrix.keys;

	(void)key;
	if (lv_group_get_focused(demo->group) != keypad)
		return false;
	for (uint32_t id = 0;; id++) {
		const char *text = lv_buttonmatrix_get_button_text(keypad, id);

		if (!text)
			return false;
		if (code == (unsigned char)text[0] && text[1] == '\0') {
			lv_buttonmatrix_set_selected_button(keypad, id);
			phone_key_typed(demo, text);
			lvgl_show_row(demo, keypad, id / 3, KEYPAD_ROWS);
			return true;
		}
	}
}

/* Calendar */

static const char *const month_names[MONTHS] = {
	N_("January"),	 N_("February"), N_("March"),	 N_("April"),
	N_("May"),	 N_("June"),	 N_("July"),	 N_("August"),
	N_("September"), N_("October"),	 N_("November"), N_("December"),
};

/* Sunday first, as the system LVGL starts the week. */
static const char *const day_names[DAYS_IN_WEEK] = {
	/* TRANSLATORS: two-letter weekday names, from Sunday. */
	N_("Su"), N_("Mo"), N_("Tu"), N_("We"), N_("Th"), N_("Fr"), N_("Sa"),
};

/* The arrow header names the month in English: name it in the language. */
/* The title between the arrow buttons of the header. */
static int32_t calendar_title_width(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);

	return content_width(demo) - (m->row_height + m->gap_small) * 2;
}

static void calendar_title(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const lv_calendar_date_t *shown =
		lv_calendar_get_showed_date(demo->u.calendar.calendar);
	lv_obj_t *title = demo->u.calendar.title;
	lv_point_t size;
	char text[64];

	if (!title || shown->month < 1 || shown->month > 12)
		return;
	snprintf(text, sizeof(text), "%s %d", _(month_names[shown->month - 1]),
		 (int)shown->year);
	lv_label_set_text(title, text);
	/* A long month wraps to a second line; the header grows to fit. */
	lv_text_get_size(&size, text, theme(demo)->font, 0, 0,
			 calendar_title_width(demo), LV_TEXT_FLAG_NONE);
	lv_obj_set_height(title, size.y);
	lv_obj_set_height(lv_obj_get_parent(title),
			  LV_MAX(m->row_height, size.y + m->gap_small * 2));
}

static void calendar_month_changed(lv_event_t *event)
{
	calendar_title(lv_event_get_user_data(event));
}

static void calendar_chosen(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_calendar_date_t date;
	struct tm when = { 0 };
	char text[64];

	if (lv_calendar_get_pressed_date(demo->u.calendar.calendar, &date) !=
	    LV_RESULT_OK)
		return;
	demo->u.calendar.chosen = date;
	lv_calendar_set_highlighted_dates(demo->u.calendar.calendar,
					  &demo->u.calendar.chosen, 1);
	when.tm_year = date.year - 1900;
	when.tm_mon = date.month - 1;
	when.tm_mday = date.day;
	when.tm_hour = 12;
	if (!fplinux_ui_format_date(demo->app->ui, mktime(&when),
				    FPLINUX_UI_DATE_SHORT, text, sizeof(text)))
		text[0] = '\0';
	lv_label_set_text_fmt(demo->status, _("Chosen: %s"), text);
}

/*
 * The calendar colours today and the chosen dates with the default theme's
 * primary colour. Its draw tasks pass this handler after its own: give
 * them the FPLinux roles instead.
 */
static void calendar_draw(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	const struct fplinux_ui_theme *t = theme(demo);
	lv_obj_t *matrix = lv_event_get_current_target_obj(event);
	lv_draw_task_t *task = lv_event_get_draw_task(event);
	lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
	lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
	lv_draw_border_dsc_t *border = lv_draw_task_get_border_dsc(task);
	lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
	uint32_t id = base->id1;
	bool focused = lv_obj_is_focused(matrix) &&
		       lv_buttonmatrix_get_selected_button(matrix) == id;

	if (base->part != LV_PART_ITEMS)
		return;
	if (fill && !focused)
		fill->opa = LV_OPA_TRANSP;
	if (border) {
		border->opa = LV_OPA_TRANSP;
		/* The chosen date: a frame, so not a colour alone. */
		if (lv_buttonmatrix_has_button_ctrl(
			    matrix, id, LV_BUTTONMATRIX_CTRL_CUSTOM_2)) {
			border->opa = LV_OPA_COVER;
			border->color = focused ? t->accent_text : t->focus;
			border->width = metrics(demo)->focus_width;
			border->side = LV_BORDER_SIDE_FULL;
		}
	}
	if (label && id < DAYS_IN_WEEK)
		label->color = t->muted;
	/*
	 * Today is underlined. Changing the font here would not enlarge the
	 * label's already calculated draw box, and two-digit days could wrap.
	 */
	if (label && lv_buttonmatrix_has_button_ctrl(
			     matrix, id, LV_BUTTONMATRIX_CTRL_CUSTOM_1))
		label->decor |= LV_TEXT_DECOR_UNDERLINE;
}

static void calendar_header_arrows(struct lvgl_demo *demo, lv_obj_t *header)
{
	static const char *const arrows[] = { "←", "→" };
	static const lv_align_t sides[] = { LV_ALIGN_LEFT_MID,
					    LV_ALIGN_RIGHT_MID };
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *buttons[] = { lv_obj_get_child(header, 0),
				lv_obj_get_child(header, 2) };
	int32_t size = m->row_height;
	lv_obj_t *title = lv_obj_get_child(header, 1);

	/* Placed, not a flex row: the title takes the room between. */
	lv_obj_set_layout(header, LV_LAYOUT_NONE);
	lv_obj_set_size(header, content_width(demo), size);
	demo->u.calendar.title = title;
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(title, calendar_title_width(demo));
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
	for (int i = 0; i < 2; i++) {
		/* The header draws LVGL symbols, which the fonts lack. */
		lv_obj_set_style_bg_image_src(buttons[i], arrows[i], 0);
		lvgl_style_key(demo, buttons[i], LV_PART_MAIN);
		lv_obj_set_size(buttons[i], size, size);
		lv_obj_align(buttons[i], sides[i], 0, 0);
		lv_obj_add_event_cb(buttons[i], calendar_month_changed,
				    LV_EVENT_CLICKED, demo);
		lvgl_add_focusable(demo, buttons[i], OK_PRESS, 0);
	}
	lv_obj_add_event_cb(header, calendar_month_changed,
			    LV_EVENT_VALUE_CHANGED, demo);
	calendar_title(demo);
}

/* The width of a dropdown that shows each of its options whole. */
static int32_t dropdown_width(struct lvgl_demo *demo, lv_obj_t *dropdown)
{
	const char *option = lv_dropdown_get_options(dropdown);
	int32_t widest = 0;
	lv_point_t size;
	char text[64];

	while (option && *option) {
		size_t length = strcspn(option, "\n");

		snprintf(text, sizeof(text), "%.*s", (int)length, option);
		lv_text_get_size(&size, text, theme(demo)->font, 0, 0,
				 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		widest = LV_MAX(widest, size.x);
		option += length + (option[length] == '\n');
	}
	lv_text_get_size(&size, lv_dropdown_get_symbol(dropdown),
			 theme(demo)->font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	/* One pixel to spare: the text and the symbol never touch. */
	return widest + size.x + 1 +
	       lv_obj_get_style_pad_column(dropdown, LV_PART_MAIN) +
	       lv_obj_get_style_pad_left(dropdown, LV_PART_MAIN) +
	       lv_obj_get_style_pad_right(dropdown, LV_PART_MAIN) +
	       lv_obj_get_style_border_width(dropdown, LV_PART_MAIN) * 2;
}

static void calendar_header_lists(struct lvgl_demo *demo, lv_obj_t *calendar,
				  lv_obj_t *header, int year)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *lists[] = { lv_obj_get_child(header, 0),
			      lv_obj_get_child(header, 1) };
	char *months = demo->u.calendar.months;
	int32_t years_width;
	int32_t months_width;
	size_t used = 0;
	int length;

	/* A short year list keeps the open list without scrolling. */
	length = snprintf(demo->u.calendar.years,
			  sizeof(demo->u.calendar.years), "%d\n%d\n%d\n%d\n%d",
			  year + 2, year + 1, year, year - 1, year - 2);
	if (length > 0)
		lv_calendar_header_dropdown_set_year_list(
			calendar, demo->u.calendar.years);
	for (int i = 0; i < MONTHS; i++) {
		length = snprintf(months + used,
				  sizeof(demo->u.calendar.months) - used,
				  i ? "\n%s" : "%s", _(month_names[i]));
		if (length < 0 ||
		    (size_t)length >= sizeof(demo->u.calendar.months) - used)
			break;
		used += (size_t)length;
	}
	lv_dropdown_set_options(lists[1], months);
	/* Select the shown year and month again after the new options. */
	lv_obj_send_event(header, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(header, m->spacing, 0);
	lv_obj_set_style_pad_row(header, m->gap_small, 0);
	for (int i = 0; i < 2; i++) {
		lv_dropdown_set_symbol(lists[i], "↓");
		lvgl_style_frame(demo, lists[i]);
		lv_obj_set_style_pad_column(lists[i], m->spacing, 0);
		lvgl_style_dropdown_list(demo, lists[i]);
		lvgl_add_focusable(demo, lists[i], OK_OPEN, USES_ARROWS);
	}
	/*
	 * Side by side, the month list as wide as its longest month, when
	 * both fit whole; otherwise one above the other, each a line wide.
	 */
	years_width = dropdown_width(demo, lists[0]);
	months_width = dropdown_width(demo, lists[1]);
	lv_obj_set_flex_grow(lists[1], 0);
	if (years_width + m->spacing + months_width <= content_width(demo)) {
		lv_obj_set_width(lists[1], months_width);
	} else {
		lv_obj_set_flex_grow(lists[0], 0);
		lv_obj_set_width(lists[0], lv_pct(100));
		lv_obj_set_width(lists[1], lv_pct(100));
	}
}

static void build_calendar(struct lvgl_demo *demo, bool lists)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	time_t now = time(NULL);
	struct tm today;
	/*
	 * In a plain box: the system's forms are flex columns in a scrolling
	 * column, and the calendar is not a form item.
	 */
	lv_obj_t *box = lvgl_box_new(demo->page);
	lv_obj_t *calendar = lv_calendar_create(box);
	lv_obj_t *header;
	lv_obj_t *matrix = lv_calendar_get_btnmatrix(calendar);
	int32_t cell = line_height(demo) + m->gap_small * 2;

	today = *localtime(&now);
	demo->u.calendar.calendar = calendar;
	for (int i = 0; i < DAYS_IN_WEEK; i++)
		demo->u.calendar.day_names[i] = _(day_names[i]);
	lv_calendar_set_day_names(calendar, demo->u.calendar.day_names);
	lv_calendar_set_today_date(calendar, today.tm_year + 1900,
				   today.tm_mon + 1, today.tm_mday);
	lv_calendar_set_month_shown(calendar, today.tm_year + 1900,
				    today.tm_mon + 1);
	header = lists ? lv_calendar_add_header_dropdown(calendar) :
			 lv_calendar_add_header_arrow(calendar);
	lvgl_untheme(calendar);
	lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_overflow_visible(box, true);
	lv_obj_set_size(calendar, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(calendar, m->gap_small, 0);
	lv_obj_set_flex_grow(matrix, 0);
	lv_obj_set_size(matrix, lv_pct(100), CALENDAR_ROWS * cell);
	lv_obj_set_style_pad_all(matrix, 0, 0);
	lv_obj_set_style_pad_gap(matrix, 0, 0);
	lv_obj_add_style(matrix, &demo->styles.cell, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(matrix, LV_OPA_TRANSP, LV_PART_ITEMS);
	lv_obj_add_style(matrix, &demo->styles.selected,
			 LV_PART_ITEMS | LV_STATE_FOCUSED);
	lv_obj_add_style(matrix, &demo->styles.disabled,
			 LV_PART_ITEMS | LV_STATE_DISABLED);
	lv_obj_add_event_cb(matrix, calendar_draw, LV_EVENT_DRAW_TASK_ADDED,
			    demo);
	lv_obj_add_event_cb(matrix, calendar_chosen, LV_EVENT_VALUE_CHANGED,
			    demo);
	/* The days first: Next goes on to the header. */
	lvgl_add_focusable(demo, matrix, OK_SELECT, USES_ARROWS);
	lvgl_show_with(demo, matrix, calendar);
	if (lists)
		calendar_header_lists(demo, calendar, header,
				      today.tm_year + 1900);
	else
		calendar_header_arrows(demo, header);
	lvgl_status_new(demo, demo->page, _("Chosen: none"));
	lvgl_note_new(
		demo, demo->page,
		lists ? _("Arrows: move between days. OK: choose. Next: the "
			  "year list, then the month list.") :
			_("Arrows: move between days. OK: choose. Next: the "
			  "month arrows."));
}

static void build_calendar_arrows(struct lvgl_demo *demo)
{
	build_calendar(demo, false);
}

static void build_calendar_lists(struct lvgl_demo *demo)
{
	build_calendar(demo, true);
}

/* Keeps the focused day in view: the days are taller than small screens. */
static bool key_calendar(struct lvgl_demo *demo,
			 const struct fplinux_ui_key *key, uint32_t code)
{
	lv_obj_t *matrix = lv_calendar_get_btnmatrix(demo->u.calendar.calendar);

	(void)key;
	if (lv_group_get_focused(demo->group) != matrix ||
	    (code != LV_KEY_UP && code != LV_KEY_DOWN && code != LV_KEY_LEFT &&
	     code != LV_KEY_RIGHT))
		return false;
	lvgl_keypad_press(demo, code);
	lvgl_show_row(demo, matrix,
		      lv_buttonmatrix_get_selected_button(matrix) /
			      DAYS_IN_WEEK,
		      CALENDAR_ROWS);
	return true;
}

static bool back_dropdown(struct lvgl_demo *demo);

/* Canvas */

static void canvas_draw(struct lvgl_demo *demo)
{
	const struct fplinux_ui_theme *t = theme(demo);
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *canvas = demo->u.canvas.canvas;
	lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(canvas);
	int32_t width = buffer->header.w, height = buffer->header.h;
	lv_layer_t layer;
	lv_draw_rect_dsc_t frame;
	lv_draw_line_dsc_t line;
	lv_draw_arc_dsc_t arc;
	lv_draw_label_dsc_t label;
	lv_area_t box = { 0, 0, width - 1, height - 1 };
	lv_area_t text = { m->spacing, m->spacing, width - 1 - m->spacing,
			   m->spacing + line_height(demo) };

	lv_canvas_fill_bg(canvas, t->background, LV_OPA_COVER);
	lv_canvas_init_layer(canvas, &layer);

	lv_draw_rect_dsc_init(&frame);
	frame.bg_opa = LV_OPA_TRANSP;
	frame.border_color = t->border;
	frame.border_width = m->focus_width;
	lv_draw_rect(&layer, &frame, &box);

	lv_draw_line_dsc_init(&line);
	line.color = t->muted;
	line.width = m->focus_width;
	line.p1.x = m->spacing;
	line.p1.y = height - m->spacing;
	line.p2.x = width / 2;
	line.p2.y = height / 2;
	lv_draw_line(&layer, &line);

	lv_draw_arc_dsc_init(&arc);
	arc.color = t->focus;
	arc.width = m->focus_width;
	arc.center.x = width * 3 / 4;
	arc.center.y = height / 2;
	arc.radius = (uint16_t)(height / 4);
	arc.start_angle = 0;
	arc.end_angle = 360;
	lv_draw_arc(&layer, &arc);

	lv_draw_label_dsc_init(&label);
	label.color = t->foreground;
	label.font = t->font;
	label.text = _("Drawn with lv_draw");
	lv_draw_label(&layer, &label, &text);

	lv_canvas_finish_layer(canvas, &layer);
	demo->u.canvas.x = width / 2;
	demo->u.canvas.y = height / 2;
}

/* The pen: a dot of focus_width pixels in the focus colour. */
static void canvas_dot(struct lvgl_demo *demo)
{
	lv_obj_t *canvas = demo->u.canvas.canvas;
	int32_t size = metrics(demo)->focus_width;

	for (int32_t y = 0; y < size; y++)
		for (int32_t x = 0; x < size; x++)
			lv_canvas_set_px(canvas, demo->u.canvas.x + x,
					 demo->u.canvas.y + y,
					 theme(demo)->focus, LV_OPA_COVER);
}

static void build_canvas(struct lvgl_demo *demo)
{
	int32_t width = content_width(demo);
	int32_t height = metrics(demo)->row_height * 3;
	lv_draw_buf_t *buffer = lvgl_keep_buffer(
		demo,
		lv_draw_buf_create(width, height, LV_COLOR_FORMAT_RGB565, 0));
	lv_obj_t *canvas;

	if (!buffer) {
		apidemo_error(demo->app, "lv_draw_buf_create");
		return;
	}
	canvas = lv_canvas_create(demo->page);
	lvgl_untheme(canvas);
	lv_canvas_set_draw_buf(canvas, buffer);
	lvgl_style_ring(demo, canvas);
	demo->u.canvas.canvas = canvas;
	canvas_draw(demo);
	canvas_dot(demo);
	lvgl_add_focusable(demo, canvas, OK_RESET, USES_ARROWS);
	lvgl_note_new(demo, demo->page,
		      _("The canvas keeps its own pixels. Arrows: draw with "
			"lv_canvas_set_px(). OK: draw it again."));
}

static bool key_canvas(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		       uint32_t code)
{
	lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(demo->u.canvas.canvas);
	int32_t step = metrics(demo)->focus_width;
	int32_t x = demo->u.canvas.x, y = demo->u.canvas.y;

	(void)key;
	if (code == LV_KEY_LEFT)
		x -= step;
	else if (code == LV_KEY_RIGHT)
		x += step;
	else if (code == LV_KEY_UP)
		y -= step;
	else if (code == LV_KEY_DOWN)
		y += step;
	else
		return false;
	if (x >= 0 && y >= 0 && x + step <= (int32_t)buffer->header.w &&
	    y + step <= (int32_t)buffer->header.h) {
		demo->u.canvas.x = x;
		demo->u.canvas.y = y;
		canvas_dot(demo);
	}
	return true;
}

static bool ok_canvas(struct lvgl_demo *demo, lv_obj_t *focused)
{
	(void)focused;
	canvas_draw(demo);
	canvas_dot(demo);
	return true;
}

/* Chart */

static void chart_new(struct lvgl_demo *demo, int index, const char *caption,
		      lv_chart_type_t type, const int32_t *values)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const struct fplinux_ui_theme *t = theme(demo);
	struct chart_view *view = &demo->u.chart.views[index];
	lv_obj_t *chart;

	lvgl_caption_new(demo, demo->page, caption);
	chart = lv_chart_create(demo->page);
	lvgl_untheme(chart);
	lv_obj_set_size(chart, lv_pct(100), m->row_height * 3);
	lv_obj_set_style_border_color(chart, t->border, 0);
	lv_obj_set_style_border_width(chart, 1, 0);
	lv_obj_set_style_border_side(chart, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_line_color(chart, t->border, 0);
	lv_obj_set_style_line_width(chart, 1, 0);
	lv_obj_set_style_line_width(chart, m->focus_width, LV_PART_ITEMS);
	lv_obj_set_style_pad_column(chart, m->gap_small, 0);
	lv_obj_set_style_width(chart, m->spacing, LV_PART_INDICATOR);
	lv_obj_set_style_height(chart, m->spacing, LV_PART_INDICATOR);
	lv_obj_set_style_radius(chart, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(chart, 1, LV_PART_CURSOR);
	lv_obj_set_style_width(chart, m->spacing * 2, LV_PART_CURSOR);
	lv_obj_set_style_height(chart, m->spacing * 2, LV_PART_CURSOR);
	lvgl_style_ring(demo, chart);
	lv_chart_set_type(chart, type);
	lv_chart_set_point_count(chart, CHART_POINTS);
	lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
	lv_chart_set_div_line_count(chart, 3, 0);
	view->chart = chart;
	view->series =
		lv_chart_add_series(chart, t->focus, LV_CHART_AXIS_PRIMARY_Y);
	for (uint32_t i = 0; i < CHART_POINTS; i++)
		lv_chart_set_series_value_by_id(chart, view->series, i,
						values[i]);
	view->cursor = lv_chart_add_cursor(chart, t->foreground,
					   LV_DIR_TOP | LV_DIR_BOTTOM);
	view->point = 0;
	lv_chart_set_cursor_point(chart, view->cursor, view->series, 0);
	lvgl_add_focusable(demo, chart, OK_NONE, USES_ARROWS | USES_TEXT);
	lvgl_show_with(demo, chart, lv_obj_get_sibling(chart, -1));
}

static void build_chart(struct lvgl_demo *demo)
{
	static const int32_t line[CHART_POINTS] = { 20, 45, 30, 70,
						    55, 80, 60, 90 };
	static const int32_t bars[CHART_POINTS] = { 60, 35, 80, 50,
						    20, 65, 40, 75 };

	chart_new(demo, 0, _("Line"), LV_CHART_TYPE_LINE, line);
	chart_new(demo, 1, _("Bars"), LV_CHART_TYPE_BAR, bars);
	lvgl_status_new(demo, demo->page, "");
	lvgl_note_new(
		demo, demo->page,
		_("A chart takes no keys; the screen moves its cursor. "
		  "Left/Right: point. Up/Down or a digit: value. Next: the "
		  "other chart."));
}

static struct chart_view *focused_chart(struct lvgl_demo *demo)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	for (int i = 0; i < CHART_COUNT; i++)
		if (demo->u.chart.views[i].chart == focused)
			return &demo->u.chart.views[i];
	return NULL;
}

static bool key_chart(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		      uint32_t code)
{
	struct chart_view *view = focused_chart(demo);
	int32_t *values;
	int32_t value;

	(void)key;
	if (!view)
		return false;
	values = lv_chart_get_series_y_array(view->chart, view->series);
	value = values[view->point];
	if (code == LV_KEY_LEFT && view->point > 0)
		view->point--;
	else if (code == LV_KEY_RIGHT && view->point + 1 < CHART_POINTS)
		view->point++;
	else if (code == LV_KEY_UP)
		value = LV_MIN(value + 5, 100);
	else if (code == LV_KEY_DOWN)
		value = LV_MAX(value - 5, 0);
	else if (code >= '0' && code <= '9')
		value = (int32_t)(code - '0') * 10;
	else if (code != LV_KEY_LEFT && code != LV_KEY_RIGHT)
		return false;
	if (code != LV_KEY_LEFT && code != LV_KEY_RIGHT)
		lv_chart_set_series_value_by_id(view->chart, view->series,
						view->point, value);
	lv_chart_set_cursor_point(view->chart, view->cursor, view->series,
				  view->point);
	lv_chart_refresh(view->chart);
	lv_label_set_text_fmt(demo->status, _("Point %u: %d"),
			      (unsigned int)view->point + 1,
			      (int)values[view->point]);
	return true;
}

/* Checkbox */

static lv_obj_t *checkbox_new(struct lvgl_demo *demo, lv_obj_t *parent,
			      const char *text, bool radio)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *box = lv_checkbox_create(parent);

	lvgl_untheme(box);
	lv_checkbox_set_text(box, text);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_style_min_height(box, m->row_height, 0);
	lv_obj_set_style_pad_hor(box, m->margin, 0);
	lv_obj_set_style_pad_ver(box, m->gap_small, 0);
	lv_obj_set_style_pad_column(box, m->spacing, 0);
	lv_obj_add_style(box, &demo->styles.selected, LV_STATE_FOCUSED);
	lv_obj_add_style(box, &demo->styles.disabled, LV_STATE_DISABLED);
	/* The marker: a frame, with ✓ or • when checked. */
	lv_obj_set_style_border_width(box, m->focus_width, LV_PART_INDICATOR);
	lv_obj_set_style_border_color(box, theme(demo)->foreground,
				      LV_PART_INDICATOR);
	lv_obj_set_style_border_color(box, theme(demo)->accent_text,
				      LV_PART_INDICATOR | LV_STATE_FOCUSED);
	lv_obj_set_style_border_color(box, theme(demo)->disabled,
				      LV_PART_INDICATOR | LV_STATE_DISABLED);
	lv_obj_set_style_bg_image_src(box, radio ? "•" : "✓",
				      LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_image_recolor(box, theme(demo)->foreground,
					  LV_PART_INDICATOR);
	lv_obj_set_style_bg_image_recolor(box, theme(demo)->accent_text,
					  LV_PART_INDICATOR | LV_STATE_FOCUSED);
	lv_obj_set_style_text_font(box, theme(demo)->font, LV_PART_INDICATOR);
	if (radio) {
		lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE,
					LV_PART_INDICATOR);
		lv_obj_set_radio_button(box, true);
	}
	return box;
}

static void build_checkbox(struct lvgl_demo *demo)
{
	lv_obj_t *list = lvgl_box_new(demo->page);
	lv_obj_t *radios;
	lv_obj_t *box;

	lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	box = checkbox_new(demo, list, _("Wi-Fi"), false);
	lv_obj_set_checked(box, true);
	lvgl_add_focusable(demo, box, OK_MARK, 0);
	box = checkbox_new(demo, list, _("Bluetooth"), false);
	lvgl_add_focusable(demo, box, OK_MARK, 0);
	box = checkbox_new(demo, list, _("Not available"), false);
	lv_obj_set_disabled(box, true);

	lvgl_caption_new(demo, demo->page, _("Radio buttons"));
	radios = lvgl_box_new(demo->page);
	lv_obj_set_size(radios, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(radios, LV_FLEX_FLOW_COLUMN);
	box = checkbox_new(demo, radios, _("Light"), true);
	lv_obj_set_checked(box, true);
	lvgl_add_focusable(demo, box, OK_SELECT, 0);
	box = checkbox_new(demo, radios, _("Dark"), true);
	lvgl_add_focusable(demo, box, OK_SELECT, 0);
	lvgl_note_new(demo, demo->page,
		      _("Up/Down: move. OK: mark or choose. A radio button "
			"unchecks its checked siblings."));
}

/* Dropdown */

static void dropdown_changed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *dropdown = lv_event_get_target_obj(event);
	char text[64];

	lv_dropdown_get_selected_str(dropdown, text, sizeof(text));
	lv_label_set_text_fmt(demo->status, _("Chosen: %s"), text);
}

static lv_obj_t *dropdown_new(struct lvgl_demo *demo, const char *caption,
			      const char *options)
{
	lv_obj_t *dropdown;

	lvgl_caption_new(demo, demo->page, caption);
	dropdown = lv_dropdown_create(demo->page);
	lvgl_untheme(dropdown);
	lv_obj_set_width(dropdown, lv_pct(100));
	lvgl_style_frame(demo, dropdown);
	lv_obj_set_style_pad_column(dropdown, metrics(demo)->spacing, 0);
	lv_dropdown_set_options(dropdown, options);
	lvgl_style_dropdown_list(demo, dropdown);
	lv_obj_add_event_cb(dropdown, dropdown_changed, LV_EVENT_VALUE_CHANGED,
			    demo);
	lvgl_add_focusable(demo, dropdown, OK_OPEN, USES_ARROWS);
	lvgl_show_with(demo, dropdown, lv_obj_get_sibling(dropdown, -1));
	return dropdown;
}

static void build_dropdown(struct lvgl_demo *demo)
{
	lv_obj_t *dropdown;

	dropdown = dropdown_new(demo, _("Fruit"),
				_("Apple\nBanana\nCherry\nLemon"));
	/* LVGL's arrow is a symbol the fonts lack; the fonts have ↓. */
	lv_dropdown_set_symbol(dropdown, "↓");
	dropdown = dropdown_new(demo, _("Custom symbol"),
				"English\nРусский\nDeutsch\nFrançais");
	/* An image symbol: a system glyph in the text colour. */
	lv_dropdown_set_symbol(dropdown, apidemo_glyph(demo->app, "language"));
	lv_obj_set_style_image_recolor(dropdown, theme(demo)->foreground,
				       LV_PART_INDICATOR);
	lv_obj_set_style_image_recolor_opa(dropdown, LV_OPA_COVER,
					   LV_PART_INDICATOR);
	lvgl_status_new(demo, demo->page, _("Chosen: none"));
	lvgl_note_new(demo, demo->page,
		      _("OK: open the list, then choose. Up/Down: move. Back: "
			"close the list."));
}

/* Back closes an open list before it leaves the screen. */
static bool back_dropdown(struct lvgl_demo *demo)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	if (!focused || !lv_obj_check_type(focused, &lv_dropdown_class) ||
	    !lv_dropdown_is_open(focused))
		return false;
	lvgl_keypad_press(demo, LV_KEY_ESC);
	lvgl_keypad_feed(demo, LV_KEY_ESC, false);
	return true;
}

/* Image */

static lv_obj_t *image_new(lv_obj_t *parent, const void *source)
{
	lv_obj_t *image = lv_image_create(parent);

	lvgl_untheme(image);
	lv_image_set_src(image, source);
	return image;
}

static void build_image(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	int32_t width = content_width(demo);
	int32_t shape = m->row_icon_size * 2;
	lv_draw_buf_t *photo = lvgl_keep_buffer(
		demo,
		fplinux_ui_image_load(APIDEMO_PHOTO, width, width * 2 / 3));
	lv_draw_buf_t *star = lvgl_keep_buffer(
		demo, fplinux_ui_image_load(APIDEMO_SHAPE, shape, shape));
	lv_obj_t *row;
	lv_obj_t *image;

	lvgl_caption_new(demo, demo->page, _("A photo, decoded once"));
	if (photo)
		image_new(demo->page, photo);
	else
		lvgl_caption_new(demo, demo->page, APIDEMO_PHOTO);
	lvgl_caption_new(demo, demo->page, _("A shape in two colours"));
	row = lvgl_row_new(demo, demo->page);
	if (star) {
		image = image_new(row, star);
		lv_obj_add_style(image, &demo->styles.glyph, 0);
		image = image_new(row, star);
		lv_obj_add_style(image, &demo->styles.glyph_focus, 0);
		lvgl_style_ring(demo, image);
		demo->u.image.star = image;
		lvgl_add_focusable(demo, image, OK_NONE, 0);
	}
	image = image_new(row, apidemo_glyph(demo->app, "theme"));
	lv_obj_add_style(image, &demo->styles.glyph, 0);
	lvgl_note_new(demo, demo->page,
		      _("fplinux_ui_image_load() decodes a PNG once, at its "
			"size. Left/Right: turn the shape with "
			"lv_image_set_rotation()."));
}

static bool key_image(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		      uint32_t code)
{
	(void)key;
	if (!demo->u.image.star ||
	    (code != LV_KEY_LEFT && code != LV_KEY_RIGHT))
		return false;
	demo->u.image.angle = (demo->u.image.angle +
			       (code == LV_KEY_LEFT ? -150 : 150) + 3600) %
			      3600;
	lv_image_set_rotation(demo->u.image.star, demo->u.image.angle);
	return true;
}

/* Image button */

/* A ring, and with filled a disc in it: the two states of a switch. */
static lv_draw_buf_t *ring_image(int32_t size, int32_t width, bool filled)
{
	lv_draw_buf_t *image =
		lv_draw_buf_create(size, size, LV_COLOR_FORMAT_A8, 0);
	int32_t centre = size / 2;
	int32_t outer = size / 2 - 1;
	int32_t inner = outer - width;
	int32_t disc = inner - width;

	if (!image)
		return NULL;
	lv_draw_buf_clear(image, NULL);
	for (int32_t y = 0; y < size; y++) {
		uint8_t *line = image->data + y * image->header.stride;

		for (int32_t x = 0; x < size; x++) {
			int32_t dx = x - centre, dy = y - centre;
			int32_t distance = dx * dx + dy * dy;

			if ((distance <= outer * outer &&
			     distance > inner * inner) ||
			    (filled && distance <= disc * disc))
				line[x] = LV_OPA_COVER;
		}
	}
	return image;
}

static void imagebutton_clicked(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);

	demo->u.imagebutton.presses++;
	lv_label_set_text_fmt(demo->status, _("Presses: %u"),
			      demo->u.imagebutton.presses);
}

static void imagebutton_toggled(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *button = lv_event_get_target_obj(event);

	lv_label_set_text(demo->status, lv_obj_is_checked(button) ?
						_("Switched on") :
						_("Switched off"));
}

static lv_obj_t *imagebutton_new(struct lvgl_demo *demo, lv_obj_t *parent,
				 const void *released, const void *checked)
{
	lv_obj_t *button = lv_imagebutton_create(parent);

	lvgl_untheme(button);
	lv_imagebutton_set_src(button, LV_IMAGEBUTTON_STATE_RELEASED, NULL,
			       released, NULL);
	lv_imagebutton_set_src(button, LV_IMAGEBUTTON_STATE_DISABLED, NULL,
			       released, NULL);
	lv_imagebutton_set_src(button, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED,
			       NULL, checked ? checked : released, NULL);
	lv_obj_add_style(button, &demo->styles.glyph_focus, 0);
	lv_obj_add_style(button, &demo->styles.disabled, LV_STATE_DISABLED);
	lvgl_style_ring(demo, button);
	return button;
}

static void build_imagebutton(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	int32_t size = m->row_icon_size * 2;
	lv_draw_buf_t *off =
		lvgl_keep_buffer(demo, ring_image(size, m->focus_width, false));
	lv_draw_buf_t *on =
		lvgl_keep_buffer(demo, ring_image(size, m->focus_width, true));
	lv_draw_buf_t *star = lvgl_keep_buffer(
		demo, fplinux_ui_image_load(APIDEMO_SHAPE, size, size));
	lv_obj_t *row = lvgl_row_new(demo, demo->page);
	lv_obj_t *button;

	if (!off || !on) {
		apidemo_error(demo->app, "lv_draw_buf_create");
		return;
	}
	if (star) {
		button = imagebutton_new(demo, row, star, NULL);
		lv_obj_add_event_cb(button, imagebutton_clicked,
				    LV_EVENT_CLICKED, demo);
		lvgl_add_focusable(demo, button, OK_PRESS, 0);
	}
	button = imagebutton_new(demo, row, off, on);
	lv_obj_set_checkable(button, true);
	lv_obj_add_event_cb(button, imagebutton_toggled, LV_EVENT_VALUE_CHANGED,
			    demo);
	lvgl_add_focusable(demo, button, OK_TOGGLE, 0);
	button = imagebutton_new(demo, row, off, on);
	lv_obj_set_disabled(button, true);
	lvgl_status_new(demo, demo->page, _("Switched off"));
	lvgl_note_new(
		demo, demo->page,
		_("An image per state: the ring is drawn in code. OK: press "
		  "or switch. Next: the next button. The third one is "
		  "disabled."));
}

/* Keyboard */

/*
 * The keyboard's own maps use LVGL symbols, which the fonts lack. These
 * maps use ← for erase, ✓ for done and a wide empty key for the space.
 */
static const char *const keys_lower[] = {
	"a", "b", "c",	"d", "e", "f",	"g",   "\n", "h", "i", "j",  "k", "l",
	"m", "n", "\n", "o", "p", "q",	"r",   "s",  "t", "u", "\n", "v", "w",
	"x", "y", "z",	",", ".", "\n", "ABC", "1#", " ", "←", "✓",  "",
};

static const char *const keys_upper[] = {
	"A", "B", "C",	"D", "E", "F",	"G",   "\n", "H", "I", "J",  "K", "L",
	"M", "N", "\n", "O", "P", "Q",	"R",   "S",  "T", "U", "\n", "V", "W",
	"X", "Y", "Z",	",", ".", "\n", "abc", "1#", " ", "←", "✓",  "",
};

static const char *const keys_special[] = {
	"1", "2", "3",	"4",  "5", "6",	 "7",	"\n",  "8", "9", "0",  "+", "-",
	"=", "?", "\n", "!",  "@", "#",	 "%",	"&",   "*", "/", "\n", "(", ")",
	":", ";", "'",	"\"", "_", "\n", "abc", "ABC", " ", "←", "✓",  "",
};

/* The widths of the keys: letters 1, the last row wider. */
static const lv_buttonmatrix_ctrl_t keys_ctrl[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 2, 4, 2, 2,
};

static void keyboard_pressed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *keyboard = lv_event_get_target_obj(event);
	lv_obj_t *textarea = demo->u.keyboard.textarea;
	uint32_t id = lv_buttonmatrix_get_selected_button(keyboard);
	const char *text = lv_buttonmatrix_get_button_text(keyboard, id);

	if (!text)
		return;
	if (!strcmp(text, "abc"))
		lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
	else if (!strcmp(text, "ABC"))
		lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
	else if (!strcmp(text, "1#"))
		lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
	else if (!strcmp(text, "←"))
		lv_textarea_delete_char(textarea);
	else if (!strcmp(text, "✓"))
		lv_label_set_text_fmt(demo->status, _("Done: %s"),
				      lv_textarea_get_text(textarea));
	else
		lv_textarea_add_text(textarea, text);
	lvgl_show_row(demo, keyboard,
		      id / DAYS_IN_WEEK < KEYBOARD_ROWS ? id / DAYS_IN_WEEK :
							  KEYBOARD_ROWS - 1,
		      KEYBOARD_ROWS);
}

static lv_obj_t *textarea_new(struct lvgl_demo *demo, lv_obj_t *parent,
			      bool one_line)
{
	lv_obj_t *textarea = lv_textarea_create(parent);

	lvgl_untheme(textarea);
	lv_obj_set_width(textarea, lv_pct(100));
	lvgl_style_frame(demo, textarea);
	lv_textarea_set_one_line(textarea, one_line);
	/* The caret: a bar of focus_width that does not blink. */
	lv_obj_set_style_border_side(textarea, LV_BORDER_SIDE_LEFT,
				     LV_PART_CURSOR);
	lv_obj_set_style_border_width(textarea, metrics(demo)->focus_width,
				      LV_PART_CURSOR);
	lv_obj_set_style_border_color(textarea, theme(demo)->foreground,
				      LV_PART_CURSOR);
	lv_obj_set_style_text_color(textarea, theme(demo)->disabled,
				    LV_PART_TEXTAREA_PLACEHOLDER);
	lv_obj_set_scrollbar_mode(textarea, LV_SCROLLBAR_MODE_OFF);
	return textarea;
}

static void build_keyboard(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *textarea = textarea_new(demo, demo->page, true);
	lv_obj_t *keyboard = lv_keyboard_create(demo->page);
	char note[256];

	demo->u.keyboard.textarea = textarea;
	/* The keyboard has the focus; the field shows only the caret. */
	lv_obj_remove_style(textarea, &demo->styles.frame_focused,
			    LV_STATE_FOCUSED);
	lv_textarea_set_placeholder_text(textarea, _("Type here"));
	lvgl_untheme(keyboard);
	/* Replace LVGL's handler, which expects its own symbols. */
	lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
	lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, keys_lower,
			    keys_ctrl);
	lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, keys_upper,
			    keys_ctrl);
	lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, keys_special,
			    keys_ctrl);
	lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
	lv_keyboard_set_textarea(keyboard, textarea);
	lv_obj_set_size(keyboard, lv_pct(100),
			KEYBOARD_ROWS * (line_height(demo) + m->gap_small * 2) +
				(KEYBOARD_ROWS - 1) * m->gap_small);
	lv_obj_set_style_pad_all(keyboard, 0, 0);
	lv_obj_set_style_pad_gap(keyboard, m->gap_small, 0);
	lv_obj_add_style(keyboard, &demo->styles.cell, LV_PART_ITEMS);
	lv_obj_set_style_pad_all(keyboard, 0, LV_PART_ITEMS);
	lv_obj_add_style(keyboard, &demo->styles.selected,
			 LV_PART_ITEMS | LV_STATE_FOCUSED);
	lv_obj_add_event_cb(keyboard, keyboard_pressed, LV_EVENT_VALUE_CHANGED,
			    demo);
	lvgl_add_focusable(demo, keyboard, OK_PRESS, USES_ARROWS);
	lvgl_show_with(demo, keyboard, textarea);
	lvgl_status_new(demo, demo->page, "");
	/* TRANSLATORS: %1$s and %2$s are the keys ← and ✓. */
	snprintf(note, sizeof(note),
		 _("Arrows: move between keys. OK: type the key. %1$s: erase, "
		   "%2$s: done. For real text use fplinux_ui_text, which has "
		   "multi-tap."),
		 "←", "✓");
	lvgl_note_new(demo, demo->page, note);
}

static bool type_keyboard_text(lv_obj_t *textarea,
			       const struct fplinux_ui_key *key)
{
	unsigned char first = (unsigned char)key->text[0];

	if (key->source != FPLINUX_UI_KEY_KEYBOARD || first < ' ' ||
	    first == 0x7f)
		return false;
	lv_textarea_add_text(textarea, key->text);
	return true;
}

/* Digits and an external keyboard type into the field directly. */
static bool key_keyboard(struct lvgl_demo *demo,
			 const struct fplinux_ui_key *key, uint32_t code)
{
	if (code == LV_KEY_BACKSPACE) {
		lv_textarea_delete_char(demo->u.keyboard.textarea);
		return true;
	}
	if (code < ' ' || code == LV_KEY_DEL)
		return false;
	if (!type_keyboard_text(demo->u.keyboard.textarea, key))
		lv_textarea_add_char(demo->u.keyboard.textarea, code);
	return true;
}

/* Label */

static void build_label(struct lvgl_demo *demo)
{
	const struct fplinux_ui_theme *t = theme(demo);
	lv_obj_t *label;

	lvgl_caption_new(demo, demo->page, _("Wrap"));
	lvgl_paragraph_new(
		demo->page,
		_("A long text breaks at spaces into as many lines as "
		  "it needs."));
	lvgl_caption_new(demo, demo->page, _("Dots"));
	lvgl_line_new(demo->page,
		      _("One line that ends with dots where it is cut"),
		      t->font);
	lvgl_caption_new(demo, demo->page, _("Scroll"));
	label = lvgl_line_new(
		demo->page,
		_("One line that scrolls round and round by itself"), t->font);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lvgl_note_new(demo, demo->page, _("Up/Down: scroll."));
}

/* LED */

/* The caption under an LED says its actual LVGL brightness. */
static void led_show(lv_obj_t *led)
{
	lv_label_set_text_fmt(lv_obj_get_user_data(led), "%u",
			      (unsigned int)lv_led_get_brightness(led));
}

static void build_led(struct lvgl_demo *demo)
{
	const struct fplinux_ui_theme *t = theme(demo);
	const lv_color_t colours[LED_COUNT] = { t->focus, t->warning,
						t->danger };
	const uint8_t brightness[LED_COUNT] = {
		LV_LED_BRIGHT_MAX,
		(LV_LED_BRIGHT_MIN + LV_LED_BRIGHT_MAX) / 2,
		LV_LED_BRIGHT_MIN,
	};
	int32_t size = metrics(demo)->row_icon_size;
	int32_t width = (content_width(demo) - metrics(demo)->spacing * 2) / 3;
	lv_obj_t *row = lvgl_row_new(demo, demo->page);
	char note[256];

	lvgl_full_width(demo, row);

	for (int i = 0; i < LED_COUNT; i++) {
		lv_obj_t *column = lvgl_column_new(demo, row, width);
		lv_obj_t *led = lv_led_create(column);

		lvgl_untheme(led);
		lv_obj_set_size(led, size, size);
		lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
		lvgl_style_ring(demo, led);
		lv_led_set_color(led, colours[i]);
		lv_led_set_brightness(led, brightness[i]);
		lv_obj_set_user_data(led, lvgl_caption_new(demo, column, ""));
		led_show(led);
		lvgl_add_focusable(demo, led, OK_TOGGLE, 0);
	}
	snprintf(note, sizeof(note),
		 _("OK: on or off. Left/Right: brightness. Next: the next LED. "
		   "LVGL's off level is %u, its on level %u."),
		 (unsigned int)LV_LED_BRIGHT_MIN,
		 (unsigned int)LV_LED_BRIGHT_MAX);
	lvgl_note_new(demo, demo->page, note);
}

static bool ok_led(struct lvgl_demo *demo, lv_obj_t *focused)
{
	(void)demo;
	lv_led_toggle(focused);
	led_show(focused);
	return true;
}

static bool key_led(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		    uint32_t code)
{
	lv_obj_t *led = lv_group_get_focused(demo->group);
	int brightness;

	(void)key;
	if (!led || (code != LV_KEY_LEFT && code != LV_KEY_RIGHT))
		return false;
	brightness =
		lv_led_get_brightness(led) + (code == LV_KEY_LEFT ? -50 : 50);
	lv_led_set_brightness(led,
			      (uint8_t)LV_CLAMP(LV_LED_BRIGHT_MIN, brightness,
						LV_LED_BRIGHT_MAX));
	led_show(led);
	return true;
}

/* Line */

static lv_obj_t *line_widget_new(struct lvgl_demo *demo,
				 const lv_point_precise_t *points,
				 uint32_t count, lv_color_t colour,
				 int32_t width)
{
	lv_obj_t *line = lv_line_create(demo->page);

	lvgl_untheme(line);
	lv_line_set_points(line, points, count);
	lv_obj_set_style_line_color(line, colour, 0);
	lv_obj_set_style_line_width(line, width, 0);
	return line;
}

static void build_line(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const struct fplinux_ui_theme *t = theme(demo);
	int32_t width = content_width(demo) - m->spacing;
	int32_t height = m->row_height * 2;
	lv_point_precise_t *zigzag = demo->u.line.zigzag;
	lv_obj_t *line;

	/* The points live as long as the line: in the demo's state. */
	for (int i = 0; i < 7; i++) {
		zigzag[i].x = m->spacing / 2 + width * i / 6;
		zigzag[i].y = i % 2 ? m->spacing : height - m->spacing;
	}
	demo->u.line.dashed[0].x = 0;
	demo->u.line.dashed[1].x = width;
	demo->u.line.thick[0].x = m->spacing;
	demo->u.line.thick[1].x = width - m->spacing;
	demo->u.line.dashed[0].y = demo->u.line.dashed[1].y = m->focus_width;
	demo->u.line.thick[0].y = demo->u.line.thick[1].y = m->spacing;

	lvgl_caption_new(demo, demo->page, _("Points"));
	line_widget_new(demo, zigzag, 7, t->focus, m->focus_width);
	lvgl_caption_new(demo, demo->page, _("Dashes"));
	line = line_widget_new(demo, demo->u.line.dashed, 2, t->foreground,
			       m->focus_width);
	lv_obj_set_style_line_dash_width(line, m->spacing, 0);
	lv_obj_set_style_line_dash_gap(line, m->spacing, 0);
	lvgl_caption_new(demo, demo->page, _("Rounded ends"));
	line = line_widget_new(demo, demo->u.line.thick, 2, t->muted,
			       m->spacing * 2);
	lv_obj_set_style_line_rounded(line, true, 0);
	lvgl_note_new(demo, demo->page, _("Up/Down: scroll."));
}

/*
 * List. LVGL 9.6 deprecates lv_list and recommends a flex column of
 * full-width buttons. The system LVGL still builds
 * lv_list, so this screen shows it; the pragma keeps the deprecation
 * warnings of its calls from stopping a -Werror build.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void row_chosen(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *row = lv_event_get_target_obj(event);

	lv_label_set_text_fmt(demo->status, _("Chosen: %s"),
			      lv_list_get_button_text(lv_obj_get_parent(row),
						      row));
}

/* A row: a button with a system glyph and a one-line text. */
static void list_row_new(struct lvgl_demo *demo, lv_obj_t *list,
			 const char *glyph, const char *text)
{
	lv_obj_t *row =
		lv_list_add_button(list, apidemo_glyph(demo->app, glyph), text);
	lv_obj_t *label = lv_obj_get_child(row, -1);

	lvgl_untheme(row);
	lv_obj_add_style(row, &demo->styles.row, 0);
	lv_obj_add_style(row, &demo->styles.selected, LV_STATE_FOCUSED);
	style_glyph_children(demo, row);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(label, line_height(demo));
	lv_obj_add_event_cb(row, row_chosen, LV_EVENT_CLICKED, demo);
	lvgl_add_focusable(demo, row, OK_SELECT, 0);
}

/* A section header of the list, in the secondary colour. */
static void list_text_new(struct lvgl_demo *demo, lv_obj_t *list,
			  const char *text)
{
	lv_obj_t *label = lv_list_add_text(list, text);

	lvgl_untheme(label);
	lv_obj_add_style(label, &demo->styles.note, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_pad_left(label, metrics(demo)->margin, 0);
	lv_obj_set_style_pad_top(label, metrics(demo)->spacing, 0);
}

static void build_list(struct lvgl_demo *demo)
{
	lv_obj_t *list = lv_list_create(demo->page);

	lvgl_untheme(list);
	/* The page scrolls; the list is as tall as its rows. */
	lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
	list_text_new(demo, list, _("Actions"));
	list_row_new(demo, list, "save", _("Save"));
	list_row_new(demo, list, "send", _("Send"));
	list_row_new(demo, list, "edit", _("Edit"));
	list_text_new(demo, list, _("Find"));
	list_row_new(demo, list, "search", _("Search"));
	list_row_new(demo, list, "help", _("Help"));
	lvgl_status_new(demo, demo->page, _("Chosen: none"));
	lvgl_caption_new(demo, demo->page, _("Deprecated in LVGL 9.6"));
	lvgl_note_new(demo, demo->page,
		      _("Build a list as a flex column of "
			"buttons. Up/Down: move. OK: choose."));
}

#pragma GCC diagnostic pop

/*
 * Menu. LVGL 9.6 deprecates lv_menu and recommends menu navigation built
 * from base widgets: pages of button rows, one
 * shown. The system LVGL still builds lv_menu, so this screen shows it; the
 * pragma keeps the deprecation warnings of its calls from stopping a
 * -Werror build. New code should build the pages itself.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/* The items of the shown page take the focus. */
static void menu_focus_page(struct lvgl_demo *demo)
{
	lv_obj_t *page = lv_menu_get_cur_main_page(demo->u.menu.menu);
	uint32_t count = page ? lv_obj_get_child_count(page) : 0;

	lvgl_clear_focusables(demo);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *child = lv_obj_get_child(page, (int32_t)i);

		if (lv_obj_is_clickable(child))
			lvgl_add_focusable(demo, child, OK_OPEN, 0);
	}
	demo->page = page;
	if (demo->focusable_count)
		lv_group_focus_obj(demo->focusables[0].object);
	lvgl_update_ok(demo);
}

static void menu_page_changed(lv_event_t *event)
{
	menu_focus_page(lv_event_get_user_data(event));
}

/* Items are rows; pages scroll with the system's indicator. */
static void menu_style(struct lvgl_demo *demo, lv_obj_t *object)
{
	uint32_t count = lv_obj_get_child_count(object);

	if (lv_obj_check_type(object, &lv_menu_cont_class) &&
	    lv_obj_is_clickable(object)) {
		lv_obj_add_style(object, &demo->styles.row, 0);
		lv_obj_add_style(object, &demo->styles.selected,
				 LV_STATE_FOCUSED);
		lv_obj_set_width(object, lv_pct(100));
	} else if (lv_obj_check_type(object, &lv_menu_cont_class)) {
		lv_obj_set_style_pad_left(object, metrics(demo)->margin, 0);
		lv_obj_set_style_pad_right(object, metrics(demo)->gutter, 0);
		lv_obj_set_style_pad_ver(object, metrics(demo)->spacing, 0);
		lv_obj_set_width(object, lv_pct(100));
	} else if (lv_obj_check_type(object, &lv_menu_page_class)) {
		apidemo_style_scroll(demo->app, object);
	}
	for (uint32_t i = 0; i < count; i++)
		menu_style(demo, lv_obj_get_child(object, (int32_t)i));
}

static void build_menu(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const struct fplinux_ui_theme *t = theme(demo);
	static const struct {
		const char *title;
		const char *text;
	} pages[] = {
		{ N_("Display"),
		  N_("A page of the menu. Back: the previous page.") },
		{ N_("Language"), N_("Each item loads its page with "
				     "lv_menu_set_load_page_event().") },
		{ N_("About"),
		  N_("lv_menu is deprecated: build menu navigation from base "
		     "widgets, pages of button rows with one shown.") },
	};
	lv_obj_t *menu;
	lv_obj_t *main_page;
	lv_obj_t *header;
	lv_obj_t *back;
	lv_obj_t *note;

	/* The menu fills the canvas: its pages scroll by themselves. */
	lv_obj_delete(demo->page);
	demo->page = NULL;
	menu = lv_menu_create(demo->area);
	demo->u.menu.menu = menu;
	lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
	main_page = lv_menu_page_create(menu, NULL);
	for (size_t i = 0; i < APIDEMO_COUNT(pages); i++) {
		lv_obj_t *page = lv_menu_page_create(menu, _(pages[i].title));
		lv_obj_t *text = lv_menu_cont_create(page);
		lv_obj_t *item = lv_menu_cont_create(main_page);
		lv_obj_t *label =
			lvgl_line_new(item, _(pages[i].title), t->font);

		lv_obj_set_width(label, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(label, 1);
		/* A column: its text wraps, as a row's would not. */
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lvgl_paragraph_new(text, _(pages[i].text));
		lv_menu_set_load_page_event(menu, item, page);
	}
	note = lv_menu_cont_create(main_page);
	lv_obj_set_flex_flow(note, LV_FLEX_FLOW_COLUMN);
	lvgl_caption_new(demo, note, _("Deprecated in LVGL 9.6"));
	lvgl_note_new(demo, note,
		      _("Up/Down: move. OK: open. Back: the previous page."));
	lv_menu_set_page(menu, main_page);
	lvgl_untheme(menu);
	menu_style(demo, menu);
	lv_obj_set_style_bg_color(menu, t->background, 0);
	lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
	/* The header of a page: a band with ← and the page's title. */
	header = lv_menu_get_main_header(menu);
	lv_obj_set_style_pad_hor(header, m->margin, 0);
	lv_obj_set_style_pad_ver(header, m->gap_small, 0);
	lv_obj_set_style_pad_column(header, m->spacing, 0);
	lv_obj_set_style_bg_color(header, t->surface, 0);
	lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(header, t->border, 0);
	lv_obj_set_style_border_width(header, 1, 0);
	lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
	back = lv_menu_get_main_header_back_button(menu);
	/* The back button shows an LVGL symbol, which the fonts lack. */
	lv_image_set_src(lv_obj_get_child(back, 0), "←");
	lv_obj_add_event_cb(menu, menu_page_changed, LV_EVENT_VALUE_CHANGED,
			    demo);
	menu_focus_page(demo);
}

/* Back: the menu's previous page, before the catalogue. */
static bool back_menu(struct lvgl_demo *demo)
{
	lv_obj_t *menu = demo->u.menu.menu;
	lv_obj_t *back = lv_menu_get_main_header_back_button(menu);

	/* The menu shows the back button below its first page only. */
	if (lv_obj_is_hidden(back))
		return false;
	lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
	return true;
}

#pragma GCC diagnostic pop

/* Message box */

static void msgbox_close(struct lvgl_demo *demo)
{
	lv_obj_t *opener = demo->u.msgbox.opener;

	if (!demo->u.msgbox.box)
		return;
	/* Delete it later: the click that closes it is still running. */
	lv_msgbox_close_async(demo->u.msgbox.box);
	demo->u.msgbox.box = NULL;
	lvgl_clear_focusables(demo);
	lvgl_add_focusable(demo, opener, OK_OPEN, 0);
	/* Restore Next's read-only key note as well as the opener. */
	lvgl_add_focusable(demo, demo->key_note, OK_NONE, 0);
	lv_group_focus_obj(opener);
}

static void msgbox_answered(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *button = lv_event_get_target_obj(event);
	lv_obj_t *label = lv_obj_get_child(button, 0);

	lv_label_set_text_fmt(demo->status, _("Answer: %s"),
			      lv_label_get_text(label));
	msgbox_close(demo);
}

static void msgbox_open(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *box = lv_msgbox_create(demo->area);
	lv_obj_t *buttons[2];
	lv_obj_t *title;

	title = lv_msgbox_add_title(box, _("Delete the photo?"));
	lv_msgbox_add_text(box, _("There is no undo."));
	buttons[0] = lv_msgbox_add_footer_button(box, _("Delete"));
	buttons[1] = lv_msgbox_add_footer_button(box, _("Cancel"));
	lvgl_untheme(box);
	lv_obj_add_style(box, &demo->styles.popup, 0);
	lv_obj_set_width(box,
			 fplinux_ui_width(demo->app->ui) - m->gap_small * 2);
	/* It fits the smallest view whole: nothing in it scrolls. */
	lv_obj_set_style_max_height(box, area_height(demo) - m->gap_small * 2,
				    0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_scrollable(lv_msgbox_get_content(box), false);
	/* LVGL makes the header and the footer a third of an inch high. */
	lv_obj_set_height(lv_msgbox_get_header(box), LV_SIZE_CONTENT);
	lv_obj_set_height(lv_msgbox_get_footer(box), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(box, m->gap_small, 0);
	lv_obj_center(box);
	lv_obj_add_style(title, &demo->styles.large, 0);
	lv_obj_set_style_pad_column(lv_msgbox_get_footer(box), m->spacing, 0);
	lv_obj_set_style_pad_top(lv_msgbox_get_footer(box), 0, 0);
	lv_obj_set_flex_align(lv_msgbox_get_footer(box), LV_FLEX_ALIGN_END,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	demo->u.msgbox.box = box;
	lvgl_clear_focusables(demo);
	for (int i = 0; i < 2; i++) {
		lvgl_style_key(demo, buttons[i], LV_PART_MAIN);
		/* Two buttons side by side in Russian too, at 128 wide. */
		lv_obj_set_style_pad_hor(buttons[i], m->gap_small, 0);
		lv_obj_set_size(buttons[i], LV_SIZE_CONTENT, m->row_height);
		lv_obj_add_event_cb(buttons[i], msgbox_answered,
				    LV_EVENT_CLICKED, demo);
		lvgl_add_focusable(demo, buttons[i], OK_SELECT, 0);
	}
	lvgl_popup_show(demo, box);
	lv_group_focus_obj(buttons[0]);
}

static void build_msgbox(struct lvgl_demo *demo)
{
	lv_obj_t *button = lvgl_button_new(demo, demo->page, _("Delete photo"));

	demo->u.msgbox.opener = button;
	lv_obj_add_event_cb(button, msgbox_open, LV_EVENT_CLICKED, demo);
	lvgl_add_focusable(demo, button, OK_OPEN, 0);
	lvgl_status_new(demo, demo->page, _("Answer: none"));
	lvgl_note_new(demo, demo->page,
		      _("OK: open the message box. In it, Left/Right: move "
			"between its buttons, OK: answer, Back: close it."));
}

/* In the box, the arrows move between its buttons. */
static bool key_msgbox(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		       uint32_t code)
{
	(void)key;
	if (!demo->u.msgbox.box)
		return false;
	if (code == LV_KEY_LEFT || code == LV_KEY_UP)
		lv_group_focus_prev(demo->group);
	else if (code == LV_KEY_RIGHT || code == LV_KEY_DOWN)
		lv_group_focus_next(demo->group);
	return true;
}

static bool back_msgbox(struct lvgl_demo *demo)
{
	if (!demo->u.msgbox.box)
		return false;
	lv_label_set_text(demo->status, _("Answer: none"));
	msgbox_close(demo);
	return true;
}

/* Roller */

/* OK sends LV_KEY_ENTER: LVGL keeps the option and reports it. */
static void roller_changed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *roller = lv_event_get_target_obj(event);
	char text[64];

	for (size_t i = 0; i < ROLLER_COUNT; i++)
		if (demo->u.roller.rollers[i] == roller)
			demo->u.roller.chosen[i] =
				lv_roller_get_selected(roller);
	lv_roller_get_selected_str(roller, text, sizeof(text));
	lv_label_set_text_fmt(demo->status, _("Chosen: %s"), text);
}

/*
 * Left without OK, a roller shows its chosen option again. LVGL reverts a
 * normal roller itself; an infinite one keeps the option turned to.
 */
static void roller_defocused(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *roller = lv_event_get_target_obj(event);

	for (size_t i = 0; i < ROLLER_COUNT; i++)
		if (demo->u.roller.rollers[i] == roller &&
		    lv_roller_get_selected(roller) != demo->u.roller.chosen[i])
			lv_roller_set_selected(roller, demo->u.roller.chosen[i],
					       LV_ANIM_OFF);
}

static lv_obj_t *roller_new(struct lvgl_demo *demo, lv_obj_t *parent,
			    size_t slot, const char *caption,
			    const char *options, lv_roller_mode_t mode)
{
	int32_t width = (content_width(demo) - metrics(demo)->spacing) / 2;
	lv_obj_t *column = lvgl_column_new(demo, parent, width);
	lv_obj_t *roller;

	lvgl_caption_new(demo, column, caption);
	roller = lv_roller_create(column);
	lvgl_untheme(roller);
	lv_obj_set_width(roller, lv_pct(100));
	lvgl_style_frame(demo, roller);
	lv_obj_set_style_pad_all(roller, 0, 0);
	lv_obj_set_style_text_line_space(roller, metrics(demo)->gap_small * 2,
					 0);
	lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(roller, &demo->styles.note, 0);
	lv_obj_set_style_bg_color(roller, theme(demo)->background, 0);
	lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_bg_color(roller, theme(demo)->surface,
				  LV_PART_SELECTED);
	lv_obj_set_style_border_side(roller,
				     LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM,
				     LV_PART_SELECTED);
	lv_obj_set_style_border_width(roller, 1, LV_PART_SELECTED);
	lv_obj_set_style_border_color(roller, theme(demo)->border,
				      LV_PART_SELECTED);
	lv_obj_set_style_text_color(roller, theme(demo)->foreground,
				    LV_PART_SELECTED);
	lv_obj_add_style(roller, &demo->styles.selected,
			 LV_PART_SELECTED | LV_STATE_FOCUSED);
	lv_roller_set_options(roller, options, mode);
	lv_roller_set_visible_row_count(roller, 3);
	demo->u.roller.rollers[slot] = roller;
	lv_obj_add_event_cb(roller, roller_changed, LV_EVENT_VALUE_CHANGED,
			    demo);
	lv_obj_add_event_cb(roller, roller_defocused, LV_EVENT_DEFOCUSED, demo);
	/* The roller takes Up and Down, and OK; Left and Right scroll nothing. */
	lvgl_add_focusable(demo, roller, OK_SELECT, USES_UP_DOWN);
	lvgl_show_with(demo, roller, column);
	return roller;
}

static void build_roller(struct lvgl_demo *demo)
{
	lv_obj_t *row = lvgl_row_new(demo, demo->page);
	char *hours = demo->u.roller.hours;
	size_t used = 0;

	for (int hour = 0; hour < 24; hour++)
		used += (size_t)snprintf(hours + used,
					 sizeof(demo->u.roller.hours) - used,
					 hour ? "\n%02d" : "%02d", hour);
	roller_new(demo, row, 0, _("Normal"),
		   _("Apple\nBanana\nCherry\nLemon\nOrange\nPlum"),
		   LV_ROLLER_MODE_NORMAL);
	roller_new(demo, row, 1, _("Infinite"), hours, LV_ROLLER_MODE_INFINITE);
	lvgl_status_new(demo, demo->page, "");
	lvgl_note_new(demo, demo->page,
		      _("Up/Down: turn the roller. OK: choose. Next: the other "
			"roller. The infinite one starts again after its last "
			"option."));
}

/* Scale */

static void build_scale(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const struct fplinux_ui_theme *t = theme(demo);
	int32_t width = content_width(demo);
	/* Fits the view with the page's padding and the focus ring. */
	int32_t size =
		LV_MIN(width, area_height(demo) - m->spacing * 2 -
				      (m->focus_width + m->gap_small) * 2);
	lv_obj_t *scale;
	lv_obj_t *needle;
	lv_scale_section_t *section;

	lvgl_caption_new(demo, demo->page, _("Horizontal, with a section"));
	scale = lv_scale_create(demo->page);
	lvgl_untheme(scale);
	lv_obj_set_size(scale, width - m->spacing * 2,
			line_height(demo) + m->spacing * 2);
	lv_scale_set_mode(scale, LV_SCALE_MODE_HORIZONTAL_BOTTOM);
	lv_scale_set_range(scale, 0, 100);
	lv_scale_set_total_tick_count(scale, 21);
	lv_scale_set_major_tick_every(scale, 5);
	lv_scale_set_label_show(scale, true);
	lv_obj_set_style_line_color(scale, t->border, LV_PART_MAIN);
	lv_obj_set_style_line_width(scale, 1, LV_PART_MAIN);
	lv_obj_set_style_line_color(scale, t->border, LV_PART_ITEMS);
	lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
	lv_obj_set_style_length(scale, m->gap_small * 2, LV_PART_ITEMS);
	lv_obj_set_style_line_color(scale, t->foreground, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
	lv_obj_set_style_length(scale, m->spacing, LV_PART_INDICATOR);
	lv_obj_set_style_text_color(scale, t->foreground, LV_PART_INDICATOR);
	/* The section from 70 draws its ticks in the warning colour. */
	section = lv_scale_add_section(scale);
	lv_scale_set_section_range(scale, section, 70, 100);
	lv_scale_set_section_style_main(scale, section,
					&demo->styles.scale_warning);
	lv_scale_set_section_style_indicator(scale, section,
					     &demo->styles.scale_warning);
	lv_scale_set_section_style_items(scale, section,
					 &demo->styles.scale_warning);

	lvgl_caption_new(demo, demo->page, _("Round, with a needle"));
	scale = lv_scale_create(demo->page);
	lvgl_untheme(scale);
	lv_obj_set_size(scale, size, size);
	lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
	lv_scale_set_range(scale, 0, 100);
	lv_scale_set_total_tick_count(scale, 11);
	lv_scale_set_major_tick_every(scale, 5);
	lv_scale_set_label_show(scale, true);
	/* A half circle leaves room between 0, 50 and 100 on a small dial. */
	lv_scale_set_angle_range(scale, 180);
	lv_scale_set_rotation(scale, 180);
	lv_obj_set_style_radius(scale, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_arc_color(scale, t->border, LV_PART_MAIN);
	lv_obj_set_style_arc_width(scale, 1, LV_PART_MAIN);
	lv_obj_set_style_line_color(scale, t->border, LV_PART_ITEMS);
	lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
	lv_obj_set_style_length(scale, m->gap_small * 2, LV_PART_ITEMS);
	lv_obj_set_style_line_color(scale, t->foreground, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
	lv_obj_set_style_length(scale, m->spacing, LV_PART_INDICATOR);
	/* LVGL adds a large built-in gap: pull labels nearer to the ticks. */
	lv_obj_set_style_pad_radial(scale, -line_height(demo) / 2,
				    LV_PART_INDICATOR);
	lvgl_style_ring(demo, scale);
	needle = lv_line_create(scale);
	lvgl_untheme(needle);
	lv_obj_set_style_line_color(needle, t->focus, 0);
	lv_obj_set_style_line_width(needle, m->focus_width, 0);
	lv_obj_set_style_line_rounded(needle, true, 0);
	demo->u.scale.round = scale;
	demo->u.scale.needle = needle;
	demo->u.scale.value = 40;
	/* Stop short of the labels: the needle never crosses their text. */
	demo->u.scale.needle_length =
		size / 2 - line_height(demo) - m->spacing - m->gap_small;
	lv_scale_set_line_needle_value(scale, needle,
				       demo->u.scale.needle_length,
				       demo->u.scale.value);
	lvgl_add_focusable(demo, scale, OK_NONE, 0);
	lvgl_show_with(demo, scale, lv_obj_get_sibling(scale, -1));
	lvgl_note_new(
		demo, demo->page,
		_("A scale takes no keys. Left/Right: move the needle with "
		  "lv_scale_set_line_needle_value()."));
}

static bool key_scale(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		      uint32_t code)
{
	(void)key;
	if (code != LV_KEY_LEFT && code != LV_KEY_RIGHT)
		return false;
	demo->u.scale.value = LV_CLAMP(
		0, demo->u.scale.value + (code == LV_KEY_LEFT ? -5 : 5), 100);
	lv_scale_set_line_needle_value(demo->u.scale.round,
				       demo->u.scale.needle,
				       demo->u.scale.needle_length,
				       demo->u.scale.value);
	return true;
}

/* Slider */

static void slider_changed(lv_event_t *event)
{
	lv_obj_t *slider = lv_event_get_target_obj(event);
	lv_obj_t *value = lv_event_get_user_data(event);
	char start[16], end[16];

	format_signed(start, sizeof(start), lv_slider_get_left_value(slider));
	format_signed(end, sizeof(end), lv_slider_get_value(slider));
	if (lv_slider_get_mode(slider) == LV_SLIDER_MODE_RANGE) {
		char range[40];

		snprintf(range, sizeof(range), "%s–%s", start, end);
		value_line_set(value, range);
	} else {
		value_line_set(value, end);
	}
}

static lv_obj_t *slider_new(struct lvgl_demo *demo, const char *caption,
			    lv_slider_mode_t mode, int32_t minimum,
			    int32_t maximum, int32_t value)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	/* Room for the knob and the ring at both ends. */
	int32_t inset = m->spacing + m->gap_small * 2 + m->focus_width;
	lv_obj_t *label = value_line_new(demo, caption);
	lv_obj_t *slider = lv_slider_create(demo->page);

	lvgl_untheme(slider);
	lv_obj_set_size(slider, content_width(demo) - inset * 2, m->spacing);
	lv_obj_add_style(slider, &demo->styles.track, LV_PART_MAIN);
	lv_obj_add_style(slider, &demo->styles.fill, LV_PART_INDICATOR);
	lv_obj_add_style(slider, &demo->styles.knob, LV_PART_KNOB);
	lv_obj_add_style(slider, &demo->styles.ring, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_pad(slider, m->spacing, LV_STATE_FOCUSED);
	lv_obj_set_style_margin_ver(slider, m->spacing, 0);
	lv_obj_set_style_margin_hor(slider, inset, 0);
	lv_slider_set_mode(slider, mode);
	lv_slider_set_range(slider, minimum, maximum);
	lv_slider_set_value(slider, value, LV_ANIM_OFF);
	lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED,
			    label);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	return slider;
}

static void build_slider(struct lvgl_demo *demo)
{
	lv_obj_t *slider;

	slider = slider_new(demo, _("Normal"), LV_SLIDER_MODE_NORMAL, 0, 10, 4);
	lvgl_add_focusable(demo, slider, OK_NONE, USES_LEFT_RIGHT);
	lvgl_show_with(demo, slider, lv_obj_get_sibling(slider, -1));
	slider = slider_new(demo, _("Symmetrical"), LV_SLIDER_MODE_SYMMETRICAL,
			    -5, 5, 2);
	lvgl_add_focusable(demo, slider, OK_NONE, USES_LEFT_RIGHT);
	lvgl_show_with(demo, slider, lv_obj_get_sibling(slider, -1));
	slider = slider_new(demo, _("Range"), LV_SLIDER_MODE_RANGE, 0, 10, 7);
	lv_slider_set_start_value(slider, 3, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	demo->u.slider.range = slider;
	/* LVGL moves the start knob only with an encoder: OK swaps knobs. */
	lvgl_add_focusable(demo, slider, OK_TOGGLE, 0);
	lvgl_show_with(demo, slider, lv_obj_get_sibling(slider, -1));
	lvgl_note_new(
		demo, demo->page,
		_("Left/Right: value. On the range slider OK picks the knob "
		  "that Left/Right move. Next: the next slider."));
}

static bool key_slider(struct lvgl_demo *demo, const struct fplinux_ui_key *key,
		       uint32_t code)
{
	lv_obj_t *slider = demo->u.slider.range;
	int32_t step = code == LV_KEY_LEFT ? -1 : 1;
	int32_t start = lv_slider_get_left_value(slider);
	int32_t end = lv_slider_get_value(slider);

	(void)key;
	if (lv_group_get_focused(demo->group) != slider ||
	    (code != LV_KEY_LEFT && code != LV_KEY_RIGHT))
		return false;
	if (demo->u.slider.start)
		start = LV_CLAMP(0, start + step, end);
	else
		end = LV_CLAMP(start, end + step, 10);
	lv_slider_set_start_value(slider, start, LV_ANIM_OFF);
	lv_slider_set_value(slider, end, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	return true;
}

static bool ok_slider(struct lvgl_demo *demo, lv_obj_t *focused)
{
	if (focused != demo->u.slider.range)
		return false;
	demo->u.slider.start = !demo->u.slider.start;
	return true;
}

/* Span group */

static lv_span_t *span_add(lv_obj_t *spans, const char *text)
{
	lv_span_t *span = lv_spangroup_add_span(spans);

	lv_span_set_text(span, text);
	return span;
}

static void build_span(struct lvgl_demo *demo)
{
	const struct fplinux_ui_theme *t = theme(demo);
	lv_obj_t *spans = lv_spangroup_create(demo->page);
	lv_obj_t *cut;
	lv_span_t *span;

	lvgl_untheme(spans);
	lv_obj_set_width(spans, lv_pct(100));
	span_add(spans, _("One paragraph, "));
	span = span_add(spans, _("several styles"));
	lv_style_set_text_font(lv_span_get_style(span), t->font_large);
	span_add(spans, _(": "));
	span = span_add(spans, _("secondary"));
	lv_style_set_text_color(lv_span_get_style(span), t->muted);
	span_add(spans, ", ");
	span = span_add(spans, _("underlined"));
	lv_style_set_text_decor(lv_span_get_style(span),
				LV_TEXT_DECOR_UNDERLINE);
	span_add(spans, _(" and "));
	span = span_add(spans, _("struck out"));
	lv_style_set_text_decor(lv_span_get_style(span),
				LV_TEXT_DECOR_STRIKETHROUGH);
	span_add(spans, _(" text."));
	lv_spangroup_refresh(spans);

	lvgl_caption_new(demo, demo->page, _("Two lines at most"));
	cut = lv_spangroup_create(demo->page);
	lvgl_untheme(cut);
	lv_obj_set_width(cut, lv_pct(100));
	lv_spangroup_set_max_lines(cut, 2);
	lv_spangroup_set_overflow(cut, LV_SPAN_OVERFLOW_ELLIPSIS);
	span_add(cut, _("A span group can stop after a number of lines and "
			"end with dots, however long its text is."));
	lv_spangroup_refresh(cut);
	lvgl_note_new(demo, demo->page, _("Up/Down: scroll."));
}

/* Spinbox */

static lv_obj_t *spinbox_new(struct lvgl_demo *demo, const char *caption,
			     uint32_t digits, uint32_t point, int32_t maximum,
			     int32_t value)
{
	lv_obj_t *spinbox;

	lvgl_caption_new(demo, demo->page, caption);
	spinbox = lv_spinbox_create(demo->page);
	lvgl_untheme(spinbox);
	lvgl_style_frame(demo, spinbox);
	lv_obj_set_width(spinbox, LV_SIZE_CONTENT);
	lv_spinbox_set_digit_count(spinbox, digits);
	lv_spinbox_set_dec_point_pos(spinbox, point);
	lv_spinbox_set_range(spinbox, 0, maximum);
	lv_spinbox_set_value(spinbox, value);
	/* The digit that Up and Down change: a selected cell. */
	lv_obj_add_style(spinbox, &demo->styles.selected, LV_PART_CURSOR);
	lv_obj_set_style_bg_opa(spinbox, LV_OPA_TRANSP, LV_PART_CURSOR);
	lv_obj_set_style_text_color(spinbox, theme(demo)->foreground,
				    LV_PART_CURSOR);
	lv_obj_add_style(spinbox, &demo->styles.selected,
			 LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_scrollbar_mode(spinbox, LV_SCROLLBAR_MODE_OFF);
	lvgl_add_focusable(demo, spinbox, OK_NONE, USES_ARROWS);
	lvgl_show_with(demo, spinbox, lv_obj_get_sibling(spinbox, -1));
	return spinbox;
}

static void build_spinbox(struct lvgl_demo *demo)
{
	spinbox_new(demo, _("Whole number"), 3, 0, 999, 42);
	spinbox_new(demo, _("Two decimals"), 5, 3, 99999, 12345);
	lvgl_note_new(
		demo, demo->page,
		_("Left/Right: digit. Up/Down: change it. Next: the other "
		  "spinbox. The spinbox always writes a point; use "
		  "fplinux_ui_format_number() for the language's own "
		  "separator."));
}

/* Spinner */

static void build_spinner(struct lvgl_demo *demo)
{
	int32_t size = metrics(demo)->row_icon_size * 2;
	lv_obj_t *row = lvgl_row_new(demo, demo->page);

	for (int i = 0; i < 2; i++) {
		lv_obj_t *spinner = lv_spinner_create(row);

		lvgl_untheme(spinner);
		lv_obj_set_size(spinner, size, size);
		lv_obj_add_style(spinner, &demo->styles.arc_track,
				 LV_PART_MAIN);
		lv_obj_add_style(spinner, &demo->styles.arc_fill,
				 LV_PART_INDICATOR);
		lv_obj_set_style_arc_width(
			spinner, metrics(demo)->focus_width * 2, LV_PART_MAIN);
		lv_obj_set_style_arc_width(spinner,
					   metrics(demo)->focus_width * 2,
					   LV_PART_INDICATOR);
		/* The second one turns slower, with a shorter arc. */
		if (i)
			lv_spinner_set_anim_params(spinner, 2000, 60);
	}
	lvgl_note_new(demo, demo->page,
		      _("A spinner turns by itself. The runtime stops it while "
			"the phone is locked. The system's busy alert turns in "
			"steps instead."));
}

/* Switch */

static void switch_row_new(struct lvgl_demo *demo, const char *text,
			   bool checked, bool disabled)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *row = lvgl_row_new(demo, demo->page);
	lv_obj_t *label = lvgl_line_new(row, text, theme(demo)->font);
	lv_obj_t *control = lv_switch_create(row);

	/* A settings row: the text at margin, the switch at the gutter. */
	lvgl_full_width(demo, row);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_width(label, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(label, 1);
	lvgl_untheme(control);
	lv_obj_set_size(control, m->row_icon_size * 2, m->row_icon_size);
	lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_add_style(control, &demo->styles.track, LV_PART_MAIN);
	lv_obj_set_style_bg_color(control, theme(demo)->accent,
				  LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(control, LV_OPA_COVER,
				LV_PART_INDICATOR | LV_STATE_CHECKED);
	/* The knob's place says on or off; its colour pairs with the fill. */
	lv_obj_set_style_bg_color(control, theme(demo)->foreground,
				  LV_PART_KNOB);
	lv_obj_set_style_bg_color(control, theme(demo)->accent_text,
				  LV_PART_KNOB | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(control, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_radius(control, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_pad_all(control, -m->gap_small, LV_PART_KNOB);
	lv_obj_set_style_bg_color(control, theme(demo)->disabled,
				  LV_PART_KNOB | LV_STATE_DISABLED);
	lv_obj_add_style(label, &demo->styles.disabled, LV_STATE_DISABLED);
	lvgl_style_ring(demo, control);
	lv_obj_set_checked(control, checked);
	if (disabled) {
		lv_obj_set_disabled(control, true);
		lv_obj_set_disabled(label, true);
		return;
	}
	/* The switch takes Left and Right too: off and on. */
	lvgl_add_focusable(demo, control, OK_TOGGLE, USES_LEFT_RIGHT);
}

static void build_switch(struct lvgl_demo *demo)
{
	switch_row_new(demo, _("Wi-Fi"), true, false);
	switch_row_new(demo, _("Bluetooth"), false, false);
	switch_row_new(demo, _("Airplane mode"), false, true);
	lvgl_note_new(
		demo, demo->page,
		_("OK: switch. Left: off, Right: on. Next: the next switch. "
		  "The third one is disabled."));
}

/* Table */

static void table_changed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *table = lv_event_get_target_obj(event);
	uint32_t row, column;

	lv_table_get_selected_cell(table, &row, &column);
	if (row == LV_TABLE_CELL_NONE || column == LV_TABLE_CELL_NONE)
		return;
	lv_label_set_text_fmt(demo->status, _("Cell: %s"),
			      lv_table_get_cell_value(table, row, column));
	lvgl_show_row(demo, table, row, TABLE_ROWS);
}

/* The header row in the secondary colour; the names to the left. */
static void table_draw(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_draw_task_t *task = lv_event_get_draw_task(event);
	lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
	lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);

	if (base->part != LV_PART_ITEMS || !label)
		return;
	/* Names left, numbers right. */
	if (base->id2 == 0)
		label->align = LV_TEXT_ALIGN_LEFT;
	if (base->id1 == 0 &&
	    !lv_obj_is_focused(lv_event_get_current_target_obj(event)))
		label->color = theme(demo)->muted;
}

/* The width of a column whose widest cell fits whole. */
static int32_t table_column_width(struct lvgl_demo *demo, lv_obj_t *table,
				  uint32_t column)
{
	int32_t widest = 0;
	lv_point_t size;

	for (uint32_t row = 0; row < TABLE_ROWS; row++) {
		lv_text_get_size(&size,
				 lv_table_get_cell_value(table, row, column),
				 theme(demo)->font, 0, 0, LV_COORD_MAX,
				 LV_TEXT_FLAG_NONE);
		widest = LV_MAX(widest, size.x);
	}
	/* The cell's padding on both sides. */
	return widest + metrics(demo)->gap_small * 4;
}

static void build_table(struct lvgl_demo *demo)
{
	static const struct {
		const char *name;
		int kilograms;
		double price;
	} fruit[TABLE_ROWS - 1] = {
		{ N_("Apple"), 2, 1.5 },
		{ N_("Banana"), 1, 0.75 },
		{ N_("Cherry"), 3, 4.2 },
		{ N_("Lemon"), 1, 0.9 },
	};
	const struct fplinux_ui_metrics *m = metrics(demo);
	int32_t width = content_width(demo);
	int32_t kilograms;
	int32_t prices;
	lv_obj_t *table = lv_table_create(demo->page);

	lvgl_untheme(table);
	lv_table_set_column_count(table, TABLE_COLUMNS);
	lv_table_set_row_count(table, TABLE_ROWS);
	lv_table_set_cell_value(table, 0, 0, _("Fruit"));
	lv_table_set_cell_value(table, 0, 1, _("kg"));
	lv_table_set_cell_value(table, 0, 2, _("Price"));
	for (int i = 0; i < TABLE_ROWS - 1; i++) {
		char price[16];

		lv_table_set_cell_value(table, i + 1, 0, _(fruit[i].name));
		lv_table_set_cell_value_fmt(table, i + 1, 1, "%d",
					    fruit[i].kilograms);
		/* The language's decimal separator: 1.50 or 1,50. */
		if (!fplinux_ui_format_number(demo->app->ui, fruit[i].price, 2,
					      price, sizeof(price)))
			price[0] = '\0';
		lv_table_set_cell_value(table, i + 1, 2, price);
	}
	/* The numbers as wide as they need, the names the rest. */
	kilograms = table_column_width(demo, table, 1);
	prices = table_column_width(demo, table, 2);
	lv_table_set_column_width(table, 1, kilograms);
	lv_table_set_column_width(table, 2, prices);
	lv_table_set_column_width(table, 0, width - kilograms - prices);
	lv_obj_set_size(table, width, LV_SIZE_CONTENT);
	lv_obj_set_scrollable(table, false);
	lv_obj_add_style(table, &demo->styles.cell, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
	lv_obj_set_style_border_side(table, LV_BORDER_SIDE_BOTTOM,
				     LV_PART_ITEMS);
	lv_obj_set_style_pad_hor(table, m->gap_small * 2, LV_PART_ITEMS);
	lv_obj_add_style(table, &demo->styles.selected,
			 LV_PART_ITEMS | LV_STATE_FOCUSED);
	lv_obj_set_style_text_align(table, LV_TEXT_ALIGN_RIGHT, LV_PART_ITEMS);
	lv_obj_add_event_cb(table, table_changed, LV_EVENT_VALUE_CHANGED, demo);
	lv_obj_add_event_cb(table, table_draw, LV_EVENT_DRAW_TASK_ADDED, demo);
	lv_obj_set_send_draw_task_events(table, true);
	lv_table_set_selected_cell(table, 1, 0);
	lvgl_add_focusable(demo, table, OK_NONE, USES_ARROWS);
	lvgl_status_new(demo, demo->page, "");
	lv_obj_send_event(table, LV_EVENT_VALUE_CHANGED, NULL);
	lvgl_note_new(demo, demo->page,
		      _("Arrows: move the selected cell. Prices use "
			"fplinux_ui_format_number()."));
}

/* Tab view */

static void build_tabview(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	static const struct {
		const char *name;
		const char *text;
	} tabs[] = {
		{ N_("Day"), N_("The first tab. Left/Right: the next tab. "
				"Up/Down: scroll the tab.") },
		{ N_("Week"), N_("The second tab. A tab view has a bar of "
				 "tab buttons and one page per tab.") },
		{ N_("Month"), N_("The third tab. Swiping between tabs needs "
				  "a touch screen; here Left and Right call "
				  "lv_tabview_set_active().") },
	};
	lv_obj_t *tabview;
	lv_obj_t *bar;

	lv_obj_delete(demo->page);
	tabview = lv_tabview_create(demo->area);
	demo->u.tabview.tabview = tabview;
	lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
	lv_tabview_set_tab_bar_size(tabview, m->row_height);
	for (size_t i = 0; i < APIDEMO_COUNT(tabs); i++) {
		lv_obj_t *tab = lv_tabview_add_tab(tabview, _(tabs[i].name));

		lvgl_paragraph_new(tab, _(tabs[i].text));
	}
	lvgl_untheme(tabview);
	bar = lv_tabview_get_tab_bar(tabview);
	lv_obj_set_style_bg_color(bar, theme(demo)->surface, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
		lv_obj_t *button = lv_obj_get_child(bar, i);

		/* Framed like keys; the shown tab is the selected cell. */
		lvgl_style_key(demo, button, LV_PART_MAIN);
		/* Three complete names at 128 wide, in Russian as well. */
		lv_obj_set_style_border_width(button, 1, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_add_style(button, &demo->styles.selected,
				 LV_STATE_CHECKED);
	}
	for (size_t i = 0; i < APIDEMO_COUNT(tabs); i++) {
		lv_obj_t *tab = lv_obj_get_child(
			lv_tabview_get_content(tabview), (int32_t)i);

		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_bg_color(tab, theme(demo)->background, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_left(tab, m->margin, 0);
		lv_obj_set_style_pad_right(tab, m->gutter, 0);
		lv_obj_set_style_pad_ver(tab, m->spacing, 0);
		apidemo_style_scroll(demo->app, tab);
	}
	lv_tabview_set_active(tabview, 0, LV_ANIM_OFF);
	demo->page = lv_obj_get_child(lv_tabview_get_content(tabview), 0);
}

static bool key_tabview(struct lvgl_demo *demo,
			const struct fplinux_ui_key *key, uint32_t code)
{
	lv_obj_t *tabview = demo->u.tabview.tabview;
	uint32_t active = lv_tabview_get_tab_active(tabview);
	uint32_t count = lv_tabview_get_tab_count(tabview);

	(void)key;
	if (code == LV_KEY_LEFT && active > 0)
		active--;
	else if (code == LV_KEY_RIGHT && active + 1 < count)
		active++;
	else
		return false;
	lv_tabview_set_active(tabview, active, LV_ANIM_OFF);
	demo->page = lv_obj_get_child(lv_tabview_get_content(tabview),
				      (int32_t)active);
	return true;
}

/* Text area */

static void textarea_ready(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *textarea = lv_event_get_target_obj(event);

	lv_label_set_text_fmt(demo->status, _("Done: %s"),
			      lv_textarea_get_text(textarea));
}

static void build_textarea(struct lvgl_demo *demo)
{
	lv_obj_t *textarea;

	lvgl_caption_new(demo, demo->page, _("One line"));
	textarea = textarea_new(demo, demo->page, true);
	lv_textarea_set_placeholder_text(textarea, _("Name"));
	lv_obj_add_event_cb(textarea, textarea_ready, LV_EVENT_READY, demo);
	lvgl_add_focusable(demo, textarea, OK_DONE,
			   USES_LEFT_RIGHT | USES_TEXT);
	lvgl_show_with(demo, textarea, lv_obj_get_sibling(textarea, -1));
	lvgl_caption_new(demo, demo->page, _("Several lines"));
	textarea = textarea_new(demo, demo->page, false);
	lv_obj_set_height(textarea, line_height(demo) * 3 +
					    metrics(demo)->frame_padding * 2);
	lv_textarea_set_text(textarea, "123\n456");
	lvgl_add_focusable(demo, textarea, OK_NEW_LINE,
			   USES_ARROWS | USES_TEXT);
	lvgl_show_with(demo, textarea, lv_obj_get_sibling(textarea, -1));
	lvgl_caption_new(demo, demo->page, _("Password"));
	textarea = textarea_new(demo, demo->page, true);
	lv_textarea_set_password_mode(textarea, true);
	lv_textarea_set_password_bullet(textarea, "•");
	lv_textarea_set_password_show_time(textarea, 0);
	lv_textarea_set_text(textarea, "1234");
	lvgl_add_focusable(demo, textarea, OK_DONE,
			   USES_LEFT_RIGHT | USES_TEXT);
	lvgl_show_with(demo, textarea, lv_obj_get_sibling(textarea, -1));
	lv_obj_add_event_cb(textarea, textarea_ready, LV_EVENT_READY, demo);
	lvgl_status_new(demo, demo->page, "");
	lvgl_note_new(demo, demo->page,
		      _("Digits: type. *: erase. Left/Right: move the caret. "
			"Next: the next field. Letters need a keyboard: use "
			"fplinux_ui_text, which has multi-tap."));
}

/* * erases on the phone, where there is no Backspace. */
static bool key_textarea(struct lvgl_demo *demo,
			 const struct fplinux_ui_key *key, uint32_t code)
{
	if (type_keyboard_text(lv_group_get_focused(demo->group), key))
		return true;
	if (code != '*' || key->source != FPLINUX_UI_KEY_PHONE)
		return false;
	lvgl_keypad_press(demo, LV_KEY_BACKSPACE);
	return true;
}

/* Tile view */

static void build_tileview(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	static const char *const texts[] = {
		N_("Tile 1 of 4. Right: the next tile. Down: the tile "
		   "below."),
		N_("Tile 2 of 4. Left: back. Down: the tile below."),
		N_("Tile 3 of 4. Up: the first tile. Right: the next tile."),
		N_("Tile 4 of 4. Swiping needs a touch screen; here the "
		   "arrows call lv_tileview_set_tile_by_index()."),
	};
	lv_obj_t *tileview;

	lv_obj_delete(demo->page);
	demo->page = NULL;
	tileview = lv_tileview_create(demo->area);
	demo->u.tileview.tileview = tileview;
	lv_obj_set_size(tileview, lv_pct(100), lv_pct(100));
	for (int i = 0; i < 4; i++) {
		lv_obj_t *tile =
			lv_tileview_add_tile(tileview, (uint8_t)(i % 2),
					     (uint8_t)(i / 2), LV_DIR_ALL);
		lv_obj_t *text;

		text = lvgl_line_new(tile, "", theme(demo)->font_large);
		lv_label_set_text_fmt(text, "%d", i + 1);
		lvgl_note_new(demo, tile, _("2/8: scroll text."));
		lvgl_paragraph_new(tile, _(texts[i]));
		lv_obj_set_scrollable(tile, true);
		apidemo_style_scroll(demo->app, tile);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_left(tile, m->margin, 0);
		lv_obj_set_style_pad_right(tile, m->gutter, 0);
		lv_obj_set_style_pad_ver(tile, m->spacing, 0);
		lv_obj_set_style_pad_row(tile, m->spacing, 0);
	}
	lvgl_untheme(tileview);
	lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
	lv_tileview_set_tile_by_index(tileview, 0, 0, LV_ANIM_OFF);
}

static bool key_tileview(struct lvgl_demo *demo,
			 const struct fplinux_ui_key *key, uint32_t code)
{
	uint32_t column = demo->u.tileview.column;
	uint32_t row = demo->u.tileview.row;

	if (code == '2' || code == '8') {
		struct fplinux_ui_key scroll = *key;

		scroll.code = code == '2' ? KEY_UP : KEY_DOWN;
		fplinux_ui_scroll_key(
			lv_tileview_get_tile_active(demo->u.tileview.tileview),
			&scroll);
		return true;
	}
	if (code == LV_KEY_LEFT && column > 0)
		column--;
	else if (code == LV_KEY_RIGHT && column < 1)
		column++;
	else if (code == LV_KEY_UP && row > 0)
		row--;
	else if (code == LV_KEY_DOWN && row < 1)
		row++;
	else
		return code == LV_KEY_LEFT || code == LV_KEY_RIGHT ||
		       code == LV_KEY_UP || code == LV_KEY_DOWN;
	lv_tileview_set_tile_by_index(demo->u.tileview.tileview, column, row,
				      LV_ANIM_OFF);
	demo->u.tileview.column = column;
	demo->u.tileview.row = row;
	return true;
}

/*
 * Window. LVGL 9.6 deprecates lv_win and recommends a flex column of a
 * header row and a content. The system LVGL still
 * builds lv_win, so this screen shows it; the pragma keeps the deprecation
 * warnings of its calls from stopping a -Werror build.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void window_button_pressed(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	const char *text = lv_obj_get_user_data(lv_event_get_target_obj(event));

	lv_label_set_text_fmt(demo->status, _("Pressed: %s"), text);
}

static void build_win(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	const struct fplinux_ui_theme *t = theme(demo);
	static const struct {
		const char *glyph;
		const char *name;
	} buttons[] = {
		{ "search", N_("Search") },
		{ "help", N_("Help") },
	};
	lv_obj_t *win;
	lv_obj_t *header;
	lv_obj_t *content;
	lv_obj_t *title;

	lv_obj_delete(demo->page);
	win = lv_win_create(demo->area);
	lv_obj_set_size(win, lv_pct(100), lv_pct(100));
	title = lv_win_add_title(win, _("Inbox"));
	for (size_t i = 0; i < APIDEMO_COUNT(buttons); i++) {
		lv_obj_t *button = lv_win_add_button(
			win, apidemo_glyph(demo->app, buttons[i].glyph),
			m->row_height);

		lv_obj_set_user_data(button, (void *)_(buttons[i].name));
		lv_obj_add_event_cb(button, window_button_pressed,
				    LV_EVENT_CLICKED, demo);
	}
	content = lv_win_get_content(win);
	lvgl_status_new(demo, content, _("Pressed: none"));
	lvgl_caption_new(demo, content, _("Deprecated in LVGL 9.6"));
	lvgl_note_new(demo, content,
		      _("Build a window as a column of a header row and a "
			"content. Next: the next header button. OK: press "
			"it. Up/Down: the next button, then scroll. "
			"Left/Right: scroll."));
	lvgl_untheme(win);
	header = lv_win_get_header(win);
	lv_obj_set_height(header, m->row_height);
	lv_obj_set_style_bg_color(header, t->surface, 0);
	lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(header, t->border, 0);
	lv_obj_set_style_border_width(header, 1, 0);
	lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_pad_left(header, m->margin, 0);
	lv_obj_set_style_pad_column(header, m->gap_small, 0);
	lv_obj_add_style(title, &demo->styles.large, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(title, lv_font_get_line_height(t->font_large));
	for (uint32_t i = 1; i < lv_obj_get_child_count(header); i++) {
		lv_obj_t *button = lv_obj_get_child(header, (int32_t)i);

		lvgl_style_key(demo, button, LV_PART_MAIN);
		style_glyph_children(demo, button);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_set_height(button, m->row_height - 1);
		lvgl_add_focusable(demo, button, OK_PRESS, 0);
	}
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_left(content, m->margin, 0);
	lv_obj_set_style_pad_right(content, m->gutter, 0);
	lv_obj_set_style_pad_ver(content, m->spacing, 0);
	lv_obj_set_style_pad_row(content, m->spacing, 0);
	apidemo_style_scroll(demo->app, content);
	demo->page = content;
}

#pragma GCC diagnostic pop

const struct widget_demo lvgl_widget_demos[WIDGET_COUNT] = {
	[WIDGET_ANIMIMG] = { .build = build_animimg, .ok = ok_animimg },
	[WIDGET_ARC] = { .build = build_arc },
	[WIDGET_ARCLABEL] = { .build = build_arclabel, .key = key_arclabel },
	[WIDGET_BAR] = { .build = build_bar, .key = key_bar },
	[WIDGET_BUTTON] = { .build = build_button },
	[WIDGET_BUTTONMATRIX] = { .build = build_buttonmatrix,
				  .key = key_buttonmatrix },
	[WIDGET_CALENDAR] = { .build = build_calendar_arrows,
			      .key = key_calendar },
	[WIDGET_CALENDAR_LISTS] = { .build = build_calendar_lists,
				    .key = key_calendar,
				    .back = back_dropdown },
	[WIDGET_CANVAS] = { .build = build_canvas,
			    .key = key_canvas,
			    .ok = ok_canvas },
	[WIDGET_CHART] = { .build = build_chart, .key = key_chart },
	[WIDGET_CHECKBOX] = { .build = build_checkbox, .rows = true },
	[WIDGET_DROPDOWN] = { .build = build_dropdown, .back = back_dropdown },
	[WIDGET_IMAGE] = { .build = build_image, .key = key_image },
	[WIDGET_IMAGEBUTTON] = { .build = build_imagebutton },
	[WIDGET_KEYBOARD] = { .build = build_keyboard, .key = key_keyboard },
	[WIDGET_LABEL] = { .build = build_label },
	[WIDGET_LED] = { .build = build_led, .key = key_led, .ok = ok_led },
	[WIDGET_LINE] = { .build = build_line },
	[WIDGET_LIST] = { .build = build_list, .rows = true },
	[WIDGET_MENU] = { .build = build_menu, .back = back_menu, .rows = true },
	[WIDGET_MSGBOX] = { .build = build_msgbox,
			    .key = key_msgbox,
			    .back = back_msgbox },
	[WIDGET_ROLLER] = { .build = build_roller },
	[WIDGET_SCALE] = { .build = build_scale, .key = key_scale },
	[WIDGET_SLIDER] = { .build = build_slider,
			    .key = key_slider,
			    .ok = ok_slider },
	[WIDGET_SPAN] = { .build = build_span },
	[WIDGET_SPINBOX] = { .build = build_spinbox },
	[WIDGET_SPINNER] = { .build = build_spinner },
	[WIDGET_SWITCH] = { .build = build_switch },
	[WIDGET_TABLE] = { .build = build_table },
	[WIDGET_TABVIEW] = { .build = build_tabview, .key = key_tabview },
	[WIDGET_TEXTAREA] = { .build = build_textarea, .key = key_textarea },
	[WIDGET_TILEVIEW] = { .build = build_tileview, .key = key_tileview },
	[WIDGET_WIN] = { .build = build_win },
};
