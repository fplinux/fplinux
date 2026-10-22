/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fplinux-ui-text.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-symbol-picker.h"
#include "fplinux-ui-text-model.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_text {
	struct fplinux_ui_text_model model;
	struct fplinux_ui *ui;
	/* Live fields, including raw fields inside a Canvas or raw screen. */
	struct fplinux_ui_text *next;
	bool menu_focused;
	bool canvas_suspended;
	bool canvas_focused;
	const struct fplinux_ui_theme *theme;
	const struct fplinux_ui_profile_spec *spec;
	lv_obj_t *frame;
	lv_obj_t *area;
	lv_obj_t *status;
	/* A read-only single line shows its text here instead of in area. */
	lv_obj_t *label;
	/* label is named to the marquee as the focused element's. */
	bool marquee;
	/* Symbol grid on the display's top layer while the picker is open. */
	struct fplinux_ui_symbol_picker *picker;
	/* Paused whenever the model has no deadline. */
	lv_timer_t *timer;
	unsigned int rendered;
	unsigned int notified;
	fplinux_ui_text_change_cb callback;
	void *context;
};

static struct fplinux_ui_text *fields;

static void render_picker(struct fplinux_ui_text *field)
{
	unsigned int index = field->model.input.picker_index;

	if (!field->model.input.picker_open) {
		fplinux_ui_symbol_picker_delete(field->picker);
		field->picker = NULL;
		return;
	}
	if (field->picker && !fplinux_ui_symbol_picker_is_open(field->picker)) {
		fplinux_ui_symbol_picker_delete(field->picker);
		field->picker = NULL;
	}
	if (!field->picker) {
		field->picker = fplinux_ui_symbol_picker_create(
			field->ui, field->frame,
			FPLINUX_UI_SYMBOL_PICKER_ANCHORED, index);
		if (!field->picker)
			fplinux_ui_text_model_cancel_picker(&field->model);
		return;
	}
	fplinux_ui_symbol_picker_select(field->picker, index);
}

static void render_selection(lv_obj_t *label, uint32_t start, uint32_t end)
{
	if (lv_label_get_text_selection_start(label) != start)
		lv_label_set_text_selection_start(label, start);
	if (lv_label_get_text_selection_end(label) != end)
		lv_label_set_text_selection_end(label, end);
}

static void show_line(struct fplinux_ui_text *field);

/*
 * The textarea scrolls to its caret with an animation; a field jumps there
 * at once, as every screen change is instant.
 */
static void finish_scroll(lv_obj_t *area)
{
	lv_point_t end;

	lv_obj_get_scroll_end(area, &end);
	lv_obj_scroll_to(area, end.x, end.y, LV_ANIM_OFF);
}

/*
 * A new size scrolls the textarea to its caret: at once, too. A larger size
 * also gives back the scroll that it no longer needs. LVGL does that only at
 * its next layout pass when a parent's layout gives the size, so a text box,
 * whose text is set before the screen has its final size, would open with
 * lines scrolled out above empty space.
 */
static void area_size_cb(lv_event_t *event)
{
	lv_obj_t *area = lv_event_get_target(event);

	finish_scroll(area);
	lv_obj_readjust_scroll(area, LV_ANIM_OFF);
}

static void render(struct fplinux_ui_text *field)
{
	const struct fplinux_ui_text_model *model = &field->model;
	const struct fplinux_ui_theme *theme = field->theme;
	lv_obj_t *label = lv_textarea_get_label(field->area);
	/* A read-only text keeps its caret at the start: it opens at the top. */
	uint32_t caret = model->read_only ?
				 0 :
				 (uint32_t)fplinux_ui_text_model_caret(model);
	bool caret_visible;
	uint32_t caret_duration;
	const char *view = fplinux_ui_text_model_view(model);
	bool reset = false;
	size_t pending;
	char status[64];

	render_picker(field);
	caret_visible = model->focused && !model->input.picker_open &&
			!model->read_only;
	/* Unchanged text and caret keep the textarea's Up/Down column. */
	if (strcmp(lv_textarea_get_text(field->area), view)) {
		lv_textarea_set_text(field->area, view);
		reset = true;
	}
	if (lv_textarea_get_cursor_pos(field->area) != caret) {
		lv_textarea_set_cursor_pos(field->area, (int32_t)caret);
		reset = true;
	}
	/*
	 * The textarea animates its scroll to the caret; a read-only text
	 * shows its start at once, without the animation to its end.
	 */
	if (reset && model->read_only) {
		lv_obj_stop_scroll_anim(field->area);
		lv_obj_scroll_to_y(field->area, 0, LV_ANIM_OFF);
	} else if (reset) {
		finish_scroll(field->area);
	}
	if (field->label && strcmp(lv_label_get_text(field->label), view))
		lv_label_set_text(field->label, view);
	show_line(field);
	if (fplinux_ui_text_model_pending(model, &pending))
		render_selection(label, (uint32_t)pending,
				 (uint32_t)pending + 1);
	else
		render_selection(label, LV_DRAW_LABEL_NO_TXT_SEL,
				 LV_DRAW_LABEL_NO_TXT_SEL);
	lv_obj_set_style_border_opa(
		field->area, caret_visible ? LV_OPA_COVER : LV_OPA_TRANSP,
		LV_PART_CURSOR);
	caret_duration = caret_visible ? 500 : 0;
	if (lv_obj_get_style_anim_duration(field->area, LV_PART_CURSOR) !=
	    caret_duration) {
		lv_obj_set_style_anim_duration(field->area, caret_duration,
					       LV_PART_CURSOR);
		/* Refresh the label too, so the textarea starts or stops blinking. */
		lv_obj_refresh_style(field->area, LV_PART_CURSOR,
				     LV_STYLE_PROP_ANY);
	}
	lv_obj_set_style_border_color(
		field->frame, model->focused ? theme->focus : theme->border, 0);
	fplinux_ui_text_model_status(model, status, sizeof(status));
	if (strcmp(lv_label_get_text(field->status), status))
		lv_label_set_text(field->status, status);
}

static void schedule(struct fplinux_ui_text *field)
{
	uint64_t deadline_ms;
	uint64_t now_ms;
	uint64_t delay_ms;

	if (!fplinux_ui_text_model_deadline(&field->model, &deadline_ms)) {
		lv_timer_pause(field->timer);
		return;
	}
	now_ms = fplinux_ui_now_ms();
	delay_ms = deadline_ms > now_ms ? deadline_ms - now_ms : 0;
	lv_timer_set_period(field->timer, delay_ms > UINT32_MAX ?
						  UINT32_MAX :
						  (uint32_t)delay_ms);
	lv_timer_reset(field->timer);
	lv_timer_resume(field->timer);
}

static void sync_view(struct fplinux_ui_text *field)
{
	if (field->rendered == field->model.revision)
		return;
	render(field);
	field->rendered = field->model.revision;
}

static void update(struct fplinux_ui_text *field)
{
	sync_view(field);
	schedule(field);
	if (field->notified == field->model.revision)
		return;
	field->notified = field->model.revision;
	if (field->callback)
		field->callback(field, field->context);
}

static void timer_cb(lv_timer_t *timer)
{
	struct fplinux_ui_text *field = lv_timer_get_user_data(timer);

	fplinux_ui_text_model_tick(&field->model, fplinux_ui_now_ms());
	update(field);
}

/* Visual lines come from the label layout, which the model cannot see. */
static bool move_line(struct fplinux_ui_text *field, bool up)
{
	uint32_t before;
	uint32_t after;

	sync_view(field);
	before = lv_textarea_get_cursor_pos(field->area);
	if (up)
		lv_textarea_cursor_up(field->area);
	else
		lv_textarea_cursor_down(field->area);
	finish_scroll(field->area);
	after = lv_textarea_get_cursor_pos(field->area);
	if (after == before)
		return false;
	fplinux_ui_text_model_set_caret(&field->model, after);
	return true;
}

/*
 * The single-line read-only label scrolls while the field has focus;
 * naming it again while it runs changes nothing.
 */
static void sync_marquee(struct fplinux_ui_text *field)
{
	if (field->label && field->model.read_only && field->model.focused) {
		fplinux_ui_marquee_focus(field->ui, field->label, NULL);
		field->marquee = true;
	} else if (field->marquee) {
		field->marquee = false;
		if (field->label)
			fplinux_ui_marquee_blur(field->ui, field->label);
	}
}

/*
 * One line shows its text in the textarea only while it is edited; at rest
 * (unfocused or read-only) a plain label shows it from its start, with
 * dots when it is longer.
 */
static void show_line(struct fplinux_ui_text *field)
{
	bool plain = field->model.read_only || !field->model.focused;

	if (!field->label)
		return;
	if (lv_obj_is_hidden(field->area) != plain)
		lv_obj_set_hidden(field->area, plain);
	if (lv_obj_is_hidden(field->label) == plain)
		lv_obj_set_hidden(field->label, !plain);
}

static void frame_delete_cb(lv_event_t *event)
{
	struct fplinux_ui_text *field = lv_event_get_user_data(event);
	struct fplinux_ui_text **link = &fields;

	fplinux_ui_cancel_text_key_repeat(field->ui, field);
	while (*link != field)
		link = &(*link)->next;
	*link = field->next;
	/* The children still exist; the label goes with the frame. */
	if (field->marquee)
		fplinux_ui_marquee_blur(field->ui, field->label);
	fplinux_ui_symbol_picker_delete(field->picker);
	lv_timer_delete(field->timer);
	fplinux_ui_text_model_cleanup(&field->model);
	free(field);
}

/*
 * Read-only hides the status row; a single line shows its text in a plain
 * label, a multi-line text scrolls with the scroll indicator
 * in a right inset that keeps the text clear of it.
 */
static void apply_read_only(struct fplinux_ui_text *field)
{
	const struct fplinux_ui_profile_spec *spec = field->spec;
	bool read_only = field->model.read_only;

	lv_obj_set_hidden(field->status, read_only);
	if (field->model.multiline) {
		fplinux_ui_style_scrollbar(field->ui, field->area);
		lv_obj_set_style_pad_ver(field->area, spec->list_padding,
					 LV_PART_SCROLLBAR);
		lv_obj_set_style_pad_right(field->area,
					   read_only ? spec->gutter : 0, 0);
		lv_obj_set_scrollbar_mode(field->area,
					  read_only ? LV_SCROLLBAR_MODE_AUTO :
						      LV_SCROLLBAR_MODE_OFF);
		return;
	}
	if (!read_only && field->marquee) {
		fplinux_ui_marquee_blur(field->ui, field->label);
		field->marquee = false;
	}
	show_line(field);
}

static void create_view(struct fplinux_ui_text *field, lv_obj_t *parent)
{
	const struct fplinux_ui_theme *theme = field->theme;
	const struct fplinux_ui_profile_spec *spec = field->spec;
	bool multiline = field->model.multiline;
	/*
	 * One line has its status right of the text where the profile has
	 * room, else on a row of its own below it, as a multi-line field.
	 */
	bool inline_status = !multiline && spec->field_status_inline;
	int32_t line = lv_font_get_line_height(theme->font);
	int32_t inset = spec->focus_width + spec->field_padding;
	lv_obj_t *label;

	field->frame = lv_obj_create(parent);
	lv_obj_remove_style_all(field->frame);
	lv_obj_set_width(field->frame, lv_pct(100));
	lv_obj_set_height(field->frame,
			  multiline ? (spec->field_lines + 1) * line +
					      spec->gap_small + 2 * inset :
				      LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(field->frame, theme->surface, 0);
	lv_obj_set_style_bg_opa(field->frame, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(field->frame, spec->focus_width, 0);
	lv_obj_set_style_pad_all(field->frame, spec->field_padding, 0);
	lv_obj_set_style_pad_row(field->frame, spec->gap_small, 0);
	lv_obj_set_style_pad_column(field->frame, spec->gap_small, 0);
	lv_obj_set_style_text_font(field->frame, theme->font, 0);
	lv_obj_set_style_text_color(field->frame, theme->foreground, 0);
	lv_obj_set_scrollable(field->frame, false);
	lv_obj_set_flex_flow(field->frame, inline_status ? LV_FLEX_FLOW_ROW :
							   LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(field->frame, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	field->area = lv_textarea_create(field->frame);
	lv_obj_remove_style_all(field->area);
	/* The native left caret needs one pixel before the first character. */
	lv_obj_set_style_pad_left(field->area, 1, 0);
	lv_textarea_set_one_line(field->area, !multiline);
	if (!inline_status)
		lv_obj_set_width(field->area, lv_pct(100));
	/* The text takes the row beside the status, or the multi-line height. */
	if (inline_status || multiline)
		lv_obj_set_flex_grow(field->area, 1);
	lv_obj_set_scrollbar_mode(field->area, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(field->area, area_size_cb, LV_EVENT_SIZE_CHANGED,
			    NULL);
	lv_obj_set_style_anim_duration(field->area, 0, LV_PART_CURSOR);
	lv_obj_set_style_border_color(field->area, theme->foreground,
				      LV_PART_CURSOR);
	lv_obj_set_style_border_width(field->area, 1, LV_PART_CURSOR);
	lv_obj_set_style_border_side(field->area, LV_BORDER_SIDE_LEFT,
				     LV_PART_CURSOR);
	label = lv_textarea_get_label(field->area);
	lv_obj_set_style_bg_color(label, theme->accent, LV_PART_SELECTED);
	lv_obj_set_style_text_color(label, theme->accent_text,
				    LV_PART_SELECTED);

	/* The text at rest, in the textarea's place. */
	if (!multiline) {
		field->label = lv_label_create(field->frame);
		if (inline_status)
			lv_obj_set_flex_grow(field->label, 1);
		else
			lv_obj_set_width(field->label, lv_pct(100));
		lv_obj_set_height(field->label, line);
		lv_obj_set_style_pad_left(field->label, 1, 0);
		lv_label_set_long_mode(field->label, LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_text(field->label, "");
		lv_obj_set_hidden(field->label, true);
	}

	field->status = lv_label_create(field->frame);
	lv_label_set_text(field->status, "");
	lv_obj_set_style_text_color(field->status, theme->muted, 0);
	if (!inline_status) {
		lv_obj_set_width(field->status, lv_pct(100));
		lv_obj_set_style_text_align(field->status, LV_TEXT_ALIGN_RIGHT,
					    0);
	}
	lv_obj_add_event_cb(field->frame, frame_delete_cb, LV_EVENT_DELETE,
			    field);
}

struct fplinux_ui_text *fplinux_ui_text_create(struct fplinux_ui *ui,
					       lv_obj_t *parent, bool multiline,
					       size_t max_chars)
{
	struct fplinux_ui_text *field = calloc(1, sizeof(*field));

	if (!field)
		return NULL;
	if (!fplinux_ui_text_model_init(&field->model, multiline, max_chars,
					FPLINUX_LANGUAGE_DIRECTORY,
					fplinux_ui_input_locale(ui))) {
		free(field);
		errno = ENOMEM;
		return NULL;
	}
	field->timer =
		lv_timer_create(timer_cb, FPLINUX_TEXT_INPUT_COMMIT_MS, field);
	if (!field->timer) {
		fplinux_ui_text_model_cleanup(&field->model);
		free(field);
		errno = ENOMEM;
		return NULL;
	}
	lv_timer_pause(field->timer);
	field->ui = ui;
	field->theme = fplinux_ui_theme(ui);
	field->spec = fplinux_ui_spec(ui);
	fplinux_ui_text_model_set_picker_columns(
		&field->model, (unsigned int)field->spec->picker_columns);
	field->rendered = UINT_MAX;
	create_view(field, parent);
	field->next = fields;
	fields = field;
	update(field);
	return field;
}

void fplinux_ui_text_delete(struct fplinux_ui_text *field)
{
	if (field)
		lv_obj_delete(field->frame);
}

lv_obj_t *fplinux_ui_text_object(struct fplinux_ui_text *field)
{
	return field->frame;
}

void fplinux_ui_text_set_change_cb(struct fplinux_ui_text *field,
				   fplinux_ui_text_change_cb callback,
				   void *context)
{
	field->callback = callback;
	field->context = context;
}

bool fplinux_ui_text_key(struct fplinux_ui_text *field,
			 const struct fplinux_ui_key *key)
{
	struct fplinux_ui_text_model_key input = {
		.keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD,
		.code = key->code,
		.pressed = key->pressed,
		.shortcut = key->control || key->alt,
		.text = key->text,
	};
	enum fplinux_ui_text_model_result result = fplinux_ui_text_model_key(
		&field->model, &input, fplinux_ui_now_ms());
	bool consumed = result == FPLINUX_UI_TEXT_MODEL_CONSUMED;

	if (result == FPLINUX_UI_TEXT_MODEL_LINE_UP ||
	    result == FPLINUX_UI_TEXT_MODEL_LINE_DOWN)
		consumed = move_line(field,
				     result == FPLINUX_UI_TEXT_MODEL_LINE_UP);
	/* A read-only text that overflows scrolls by line and by page. */
	else if (!consumed && field->model.read_only &&
		 field->model.multiline && field->model.focused)
		consumed = fplinux_ui_scroll_key(field->area, key);
	if (consumed && key->pressed &&
	    fplinux_ui_text_can_repeat(field->ui, field))
		fplinux_ui_claim_text_key(field->ui, field, key);
	update(field);
	return consumed;
}

bool fplinux_ui_text_can_repeat(const struct fplinux_ui *ui,
				const struct fplinux_ui_text *field)
{
	return field && field->ui == ui && field->model.focused &&
	       lv_obj_is_visible(field->frame);
}

const char *fplinux_ui_text_get(const struct fplinux_ui_text *field)
{
	return field->model.display;
}

bool fplinux_ui_text_set(struct fplinux_ui_text *field, const char *text)
{
	if (!fplinux_ui_text_model_set_text(&field->model, text))
		return false;
	update(field);
	/* A focused read-only line scrolls its new text from the start. */
	if (field->marquee)
		fplinux_ui_marquee_changed(field->ui, field->label, NULL);
	return true;
}

bool fplinux_ui_text_set_constraint(struct fplinux_ui_text *field,
				    enum fplinux_ui_text_constraint constraint)
{
	static const enum fplinux_ui_text_model_type types[] = {
		[FPLINUX_UI_TEXT_ANY] = FPLINUX_UI_TEXT_MODEL_ANY,
		[FPLINUX_UI_TEXT_NUMERIC] = FPLINUX_UI_TEXT_MODEL_NUMERIC,
		[FPLINUX_UI_TEXT_DECIMAL] = FPLINUX_UI_TEXT_MODEL_DECIMAL,
		[FPLINUX_UI_TEXT_PHONE] = FPLINUX_UI_TEXT_MODEL_PHONE,
	};
	unsigned int type = (unsigned int)constraint &
			    ~(unsigned int)(FPLINUX_UI_TEXT_PASSWORD |
					    FPLINUX_UI_TEXT_UNEDITABLE);
	bool read_only = constraint & FPLINUX_UI_TEXT_UNEDITABLE;

	if (type >= sizeof(types) / sizeof(types[0])) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_text_model_set_type(
		    &field->model, types[type],
		    fplinux_ui_decimal_separator(field->ui)))
		return false;
	fplinux_ui_text_model_set_password(
		&field->model, constraint & FPLINUX_UI_TEXT_PASSWORD);
	if (read_only != field->model.read_only) {
		fplinux_ui_text_model_set_read_only(&field->model, read_only);
		apply_read_only(field);
	}
	update(field);
	sync_marquee(field);
	return true;
}

bool fplinux_ui_text_is_read_only(const struct fplinux_ui_text *field)
{
	return field->model.read_only;
}

bool fplinux_ui_text_takes_newline(const struct fplinux_ui_text *field)
{
	return fplinux_ui_text_model_takes_newline(&field->model);
}

bool fplinux_ui_text_is_empty(const struct fplinux_ui_text *field)
{
	return fplinux_ui_text_model_is_empty(&field->model);
}

const char *fplinux_ui_text_indicator(const struct fplinux_ui_text *field)
{
	return fplinux_ui_text_model_indicator(&field->model);
}

bool fplinux_ui_text_is_picker_open(const struct fplinux_ui_text *field)
{
	return field->model.input.picker_open;
}

void fplinux_ui_text_cancel_input(struct fplinux_ui *ui,
				  enum fplinux_ui_key_source source)
{
	if (source != FPLINUX_UI_KEY_PHONE)
		return;
	for (struct fplinux_ui_text *field = fields; field;
	     field = field->next) {
		if (field->ui != ui)
			continue;
		fplinux_ui_text_model_cancel_held(&field->model);
		update(field);
	}
}

/* Canvas fields also lose focus while another screen or alert is on top. */
void fplinux_ui_text_canvas_active(lv_obj_t *canvas, bool active)
{
	for (struct fplinux_ui_text *field = fields; field;
	     field = field->next) {
		lv_obj_t *parent = field->frame;

		while (parent && parent != canvas)
			parent = lv_obj_get_parent(parent);
		if (!parent || field->canvas_suspended == !active)
			continue;
		field->canvas_suspended = !active;
		if (!active) {
			field->canvas_focused = field->model.focused;
			fplinux_ui_text_set_focused(field, false);
		} else if (field->canvas_focused &&
			   lv_obj_is_visible(field->frame)) {
			fplinux_ui_text_set_focused(field, true);
		}
	}
}

/* Menus also cover raw/Canvas fields: held editing must stop underneath. */
void fplinux_ui_text_menu_suspend(struct fplinux_ui *ui)
{
	for (struct fplinux_ui_text *field = fields; field;
	     field = field->next) {
		if (field->ui != ui)
			continue;
		field->menu_focused = field->model.focused &&
				      lv_obj_is_visible(field->frame);
		if (field->menu_focused)
			fplinux_ui_text_set_focused(field, false);
	}
}

void fplinux_ui_text_menu_reclaim(struct fplinux_ui *ui)
{
	for (struct fplinux_ui_text *field = fields; field;
	     field = field->next) {
		if (field->ui != ui || !field->menu_focused)
			continue;
		field->menu_focused = false;
		/* A field deleted with its parent is no longer in the live list. */
		if (lv_obj_is_visible(field->frame))
			fplinux_ui_text_set_focused(field, true);
	}
}

/* An open grid is drawn again over its field, inside the new display. */
void fplinux_ui_text_resize(struct fplinux_ui *ui)
{
	for (struct fplinux_ui_text *field = fields; field;
	     field = field->next) {
		if (field->ui != ui || !field->picker)
			continue;
		if (!fplinux_ui_symbol_picker_refresh(field->picker)) {
			fplinux_ui_text_model_cancel_picker(&field->model);
			update(field);
		}
	}
}

void fplinux_ui_text_set_focused(struct fplinux_ui_text *field, bool focused)
{
	if (!focused)
		fplinux_ui_cancel_text_key_repeat(field->ui, field);
	fplinux_ui_text_model_set_focused(&field->model, focused);
	update(field);
	show_line(field);
	sync_marquee(field);
}
