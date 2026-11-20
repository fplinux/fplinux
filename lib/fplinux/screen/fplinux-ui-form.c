/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-screen-private.h"
#include "fplinux-ui-listview.h"
#include "fplinux-ui-locale.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A date item's value as fplinux_ui_format_date() writes it. */
#define DATE_VALUE_BYTES 128
/* A popup choice's private Back command. */
#define POPUP_BACK_COMMAND (-3)

enum item_kind {
	ITEM_STRING,
	ITEM_VALUE,
	ITEM_TEXT,
	ITEM_CHOICE,
	ITEM_GAUGE,
	ITEM_DATE,
	ITEM_DURATION,
	ITEM_IMAGE,
};

/* How focus enters a choice's rows. */
enum entry {
	ENTER_TOP,
	ENTER_BOTTOM,
	ENTER_KEEP,
};

struct form_item {
	struct fplinux_ui_screen *form;
	size_t index;
	enum item_kind kind;
	lv_obj_t *box;
	lv_obj_t *label;
	/* A gauge's one-line name. */
	lv_obj_t *gauge_name;
	/* String text, gauge number, date text or image. */
	lv_obj_t *value;
	/*
	 * The border of a gauge or date that shows the focus, or the box of a
	 * link or button around its value.
	 */
	lv_obj_t *frame;
	lv_obj_t *bar;
	struct fplinux_ui_text *field;
	enum fplinux_ui_text_constraint constraint;
	/* A DECIMAL field's text with '.', as the application reads it. */
	char *content;
	/* The text last reported, and whether a report is queued. */
	char *reported;
	bool report_pending;
	/* Set while the application changes the text itself. */
	bool quiet;
	struct fplinux_ui_listview *list;
	enum fplinux_ui_choice_kind choice_kind;
	char **choices;
	size_t count;
	size_t chosen;
	/* A popup choice's label, also the title of its list. */
	char *name;
	int minimum;
	int maximum;
	int number;
	bool interactive;
	enum fplinux_ui_date_mode date_mode;
	time_t date;
	unsigned int duration_seconds;
	/* Takes no space and never has focus. */
	bool hidden;
	/* A stacked read-only property, using the ordinary string contract. */
	bool property;
	/* The visible trailing line of a property or value row. */
	bool divider;
	/* A value's geometry callback may change its own height. */
	bool value_layout;
	/* A string or image item drawn as a link or button with commands. */
	enum fplinux_ui_appearance appearance;
	/* An image's decoded picture, and a copy that fits inside the box. */
	lv_draw_buf_t *image;
	lv_draw_buf_t *fitted;
	bool image_fitting;
};

static struct form_item *focused_item(const struct fplinux_ui_screen *screen)
{
	if ((screen->kind != SCREEN_FORM && screen->kind != SCREEN_TEXT_BOX) ||
	    screen->form.focus == NONE)
		return NULL;
	return screen->form.items[screen->form.focus];
}

struct fplinux_ui_text *
fplinux_ui_form_focused_text(const struct fplinux_ui_screen *screen)
{
	struct form_item *item = focused_item(screen);

	return item && item->kind == ITEM_TEXT ? item->field : NULL;
}

static bool segmented(const struct form_item *item)
{
	return item->kind == ITEM_DATE || item->kind == ITEM_DURATION;
}

static bool inline_choice(const struct form_item *item)
{
	return item->kind == ITEM_CHOICE &&
	       item->choice_kind != FPLINUX_UI_CHOICE_POPUP;
}

/* Form focus */

/* A gauge's name or a segmented value scrolls while it has focus. */
static lv_obj_t *marquee_label(const struct form_item *item)
{
	if (item->kind == ITEM_GAUGE)
		return item->gauge_name;
	return segmented(item) ? item->value : NULL;
}

/*
 * A string or image item drawn as a link or button: it takes the focus,
 * and OK runs its first item command, only while it has item commands.
 */
static bool actionable(const struct form_item *item)
{
	if (item->kind == ITEM_VALUE)
		return fplinux_ui_screen_has_item_commands(item->form,
							   item->index);
	return (item->kind == ITEM_STRING || item->kind == ITEM_IMAGE) &&
	       item->appearance != FPLINUX_UI_APPEARANCE_PLAIN &&
	       fplinux_ui_screen_has_item_commands(item->form, item->index);
}

/*
 * A focused link is filled with the accent colour, as a focused row is; a
 * button is a framed control, whose frame turns to the focus colour.
 */
static void show_appearance_focus(struct form_item *item, bool focused)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	lv_color_t colour = focused ? theme->accent_text : theme->accent;

	if (item->kind == ITEM_VALUE) {
		lv_obj_set_style_bg_color(item->box, theme->accent, 0);
		lv_obj_set_style_bg_opa(
			item->box, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		if (item->label)
			lv_obj_set_style_text_color(
				item->label,
				focused ? theme->accent_text : theme->muted, 0);
		lv_obj_set_style_text_color(
			item->value,
			focused ? theme->accent_text : theme->foreground, 0);
		return;
	}
	if (item->appearance == FPLINUX_UI_APPEARANCE_BUTTON) {
		fplinux_ui_control_focus(item->form->ui, item->frame, focused);
		if (item->kind == ITEM_IMAGE)
			lv_obj_set_style_image_recolor(
				item->value,
				focused ? theme->accent_text :
					  theme->foreground,
				0);
		else
			lv_obj_set_style_text_color(item->value,
						    focused ?
							    theme->accent_text :
							    theme->foreground,
						    0);
		return;
	}
	lv_obj_set_style_bg_opa(item->frame,
				focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (item->kind == ITEM_IMAGE)
		lv_obj_set_style_image_recolor(item->value, colour, 0);
	else
		lv_obj_set_style_text_color(item->value, colour, 0);
}

/*
 * Draws the focus of an active form; text fields and choices take the
 * marquee themselves, gauges and dates through marquee_label().
 */
void fplinux_ui_form_apply_focus(struct fplinux_ui_screen *form, bool active)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(form->ui);
	lv_obj_t *marquee = NULL;

	for (size_t i = 0; i < form->form.item_count; ++i) {
		struct form_item *item = form->form.items[i];
		bool focused = active && i == form->form.focus;
		lv_obj_t *label = marquee_label(item);

		if (item->kind == ITEM_TEXT)
			fplinux_ui_text_set_focused(item->field, focused);
		else if (item->kind == ITEM_CHOICE)
			fplinux_ui_listview_set_selectable(item->list, focused);
		else if (item->kind == ITEM_VALUE || actionable(item))
			show_appearance_focus(item, focused);
		else if (item->frame &&
			 (segmented(item) || item->interactive)) {
			fplinux_ui_control_focus(form->ui, item->frame,
						 focused);
			lv_obj_set_style_text_color(
				item->value,
				focused ? theme->accent_text :
					  (item->kind == ITEM_GAUGE ?
						   theme->muted :
						   theme->foreground),
				0);
			if (segmented(item)) {
				lv_obj_set_style_bg_color(item->value,
							  theme->surface,
							  LV_PART_SELECTED);
				lv_obj_set_style_text_color(item->value,
							    theme->foreground,
							    LV_PART_SELECTED);
			} else if (item->kind == ITEM_GAUGE) {
				lv_obj_set_style_text_color(
					item->gauge_name,
					focused ? theme->accent_text :
						  theme->foreground,
					0);
				lv_obj_set_style_bg_color(
					item->bar,
					focused ? theme->accent_text :
						  theme->focus,
					LV_PART_INDICATOR);
			}
		}
		if (label && focused)
			marquee = label;
		else if (label)
			fplinux_ui_marquee_blur(form->ui, label);
	}
	if (marquee)
		fplinux_ui_marquee_focus(form->ui, marquee, NULL);
}

static void view_area(lv_obj_t *content, lv_area_t *view)
{
	lv_obj_get_coords(content, view);
	view->y1 += lv_obj_get_style_pad_top(content, LV_PART_MAIN);
	view->y2 -= lv_obj_get_style_pad_bottom(content, LV_PART_MAIN);
}

static bool item_visible(const struct form_item *item)
{
	return !lv_obj_is_hidden(item->box);
}

static lv_obj_t *reading_edge(const struct form_item *item, bool down)
{
	lv_obj_t *label = down ? item->value : item->label;

	if (!label || lv_obj_is_hidden(label))
		label = down ? item->label : item->value;
	return label && !lv_obj_is_hidden(label) ? label : NULL;
}

/* Finish a partly clipped end line, including the content's edge padding. */
static void settle_form_text_edge(struct fplinux_ui_screen *form, bool down)
{
	struct form_item *item = NULL;
	lv_obj_t *label;
	lv_area_t view, text;
	int32_t line, clipped;

	for (size_t n = 0; n < form->form.item_count; ++n) {
		size_t i = down ? form->form.item_count - 1 - n : n;

		if (item_visible(form->form.items[i])) {
			item = form->form.items[i];
			break;
		}
	}
	if (!item || (item->kind != ITEM_STRING && item->kind != ITEM_VALUE) ||
	    (item->kind == ITEM_STRING && actionable(item)))
		return;
	label = reading_edge(item, down);
	if (!label)
		return;
	lv_obj_update_layout(form->content);
	view_area(form->content, &view);
	lv_obj_get_coords(label, &text);
	line = lv_font_get_line_height(lv_obj_get_style_text_font(label, 0));
	clipped = down ? text.y2 - view.y2 : view.y1 - text.y1;
	if (line > 0 && clipped > 0 && clipped < line)
		lv_obj_scroll_to_y(form->content,
				   down ? lv_obj_get_scroll_y(form->content) +
						   lv_obj_get_scroll_bottom(
							   form->content) :
					  0,
				   LV_ANIM_OFF);
}

/* Scroll one normal step, completing a clipped line at either end. */
static void scroll_form(struct fplinux_ui_screen *form, int32_t dy)
{
	lv_obj_scroll_by_bounded(form->content, 0, dy, LV_ANIM_OFF);
	settle_form_text_edge(form, dy < 0);
}

/* Scrolls the least that shows area, or its top when it is taller. */
static void reveal(struct fplinux_ui_screen *form, const lv_area_t *area)
{
	lv_area_t view;
	int32_t dy;

	view_area(form->content, &view);
	dy = fplinux_ui_reveal_scroll(view.y1, view.y2, area->y1, area->y2,
				      fplinux_ui_screen_spec(form)->row_height);
	if (dy)
		scroll_form(form, dy);
}

/* The focused row of an inline choice, or the whole item. */
static void item_area(const struct form_item *item, lv_area_t *area)
{
	if (inline_choice(item) && fplinux_ui_listview_count(item->list)) {
		lv_obj_t *rows = fplinux_ui_listview_object(item->list);

		lv_obj_get_coords(
			lv_obj_get_child(rows,
					 (int32_t)fplinux_ui_listview_selected(
						 item->list)),
			area);
		return;
	}
	lv_obj_get_coords(item->box, area);
}

static bool focusable(const struct form_item *item)
{
	return item_visible(item) &&
	       (item->kind == ITEM_TEXT || item->kind == ITEM_CHOICE ||
		segmented(item) ||
		(item->kind == ITEM_GAUGE && item->interactive) ||
		actionable(item));
}

static void form_focus(struct fplinux_ui_screen *form, size_t index,
		       enum entry entry)
{
	struct form_item *item = form->form.items[index];
	lv_area_t area;

	form->form.focus = index;
	if (inline_choice(item) && entry != ENTER_KEEP)
		fplinux_ui_listview_select(
			item->list, entry == ENTER_TOP ? 0 : item->count - 1);
	fplinux_ui_form_apply_focus(form, fplinux_ui_screen_is_top(form));
	lv_obj_update_layout(form->content);
	item_area(item, &area);
	if (item->kind == ITEM_VALUE && entry == ENTER_BOTTOM) {
		lv_area_t view;

		view_area(form->content, &view);
		if (lv_area_get_height(&area) > lv_area_get_height(&view))
			area.y1 = area.y2 - lv_area_get_height(&view) + 1;
	}
	reveal(form, &area);
}

/*
 * When a form first shows a focusable item, that item takes the focus if it
 * is in sight; otherwise Down reads up to it.
 */
void fplinux_ui_form_settle(struct fplinux_ui_screen *form)
{
	lv_area_t view;
	lv_area_t area;

	if (form->kind != SCREEN_FORM || form->form.settled)
		return;
	for (size_t i = 0; i < form->form.item_count; ++i) {
		if (!focusable(form->form.items[i]))
			continue;
		form->form.settled = true;
		lv_obj_update_layout(form->content);
		view_area(form->content, &view);
		lv_obj_get_coords(form->form.items[i]->box, &area);
		if (area.y1 <= view.y2 && area.y2 >= view.y1)
			form_focus(form, i, ENTER_TOP);
		return;
	}
}

/*
 * The next focusable item that way, or from the view without a focus: one
 * whose leading edge is in sight or still ahead, never one that a scroll
 * has partly passed (form_move()).
 */
static size_t neighbour(const struct fplinux_ui_screen *form, bool down,
			const lv_area_t *view)
{
	lv_area_t area;

	if (form->form.focus != NONE) {
		for (size_t i = form->form.focus;
		     down ? i + 1 < form->form.item_count : i > 0;) {
			i = down ? i + 1 : i - 1;
			if (focusable(form->form.items[i]))
				return i;
		}
		return NONE;
	}
	for (size_t n = 0; n < form->form.item_count; ++n) {
		size_t i = down ? n : form->form.item_count - 1 - n;

		if (!focusable(form->form.items[i]))
			continue;
		lv_obj_get_coords(form->form.items[i]->box, &area);
		if (down ? area.y1 >= view->y1 : area.y2 <= view->y2)
			return i;
	}
	return NONE;
}

/* The first item met going that way: after the focus, or the first in sight. */
static long first_passed(const struct fplinux_ui_screen *form, bool down,
			 const lv_area_t *view)
{
	long count = (long)form->form.item_count;
	lv_area_t area;

	if (form->form.focus != NONE)
		return (long)form->form.focus + (down ? 1 : -1);
	for (long n = 0; n < count; ++n) {
		long i = down ? n : count - 1 - n;

		if (!item_visible(form->form.items[i]))
			continue;
		lv_obj_get_coords(form->form.items[i]->box, &area);
		if (down ? area.y2 > view->y1 : area.y1 < view->y2)
			return i;
	}
	return down ? count : -1;
}

/* True when read-only content before the next item is not yet in sight. */
static bool unread(const struct fplinux_ui_screen *form, bool down, size_t next,
		   const lv_area_t *view)
{
	long end = next != NONE ? (long)next :
		   down		? (long)form->form.item_count :
				  -1;
	lv_area_t area;

	for (long i = first_passed(form, down, view); i != end;
	     i += down ? 1 : -1) {
		if (focusable(form->form.items[i]) ||
		    !item_visible(form->form.items[i]))
			continue;
		lv_obj_get_coords(form->form.items[i]->box, &area);
		if (down ? area.y2 > view->y2 : area.y1 < view->y1)
			return true;
	}
	return false;
}

static void blur_scrolled_item(struct fplinux_ui_screen *form,
			       struct form_item *item, int32_t previous_top)
{
	lv_area_t view;
	lv_area_t area;

	lv_obj_get_coords(form->content, &view);
	item_area(item, &area);
	if (item->kind == ITEM_VALUE && area.y1 <= view.y2 &&
	    area.y2 >= view.y1)
		return;
	if (area.y1 != previous_top &&
	    (area.y1 < view.y1 || area.y2 > view.y2)) {
		form->form.focus = NONE;
		fplinux_ui_form_apply_focus(form, true);
	}
}

/*
 * Up and Down step through a choice's rows, then to the next focusable item;
 * read-only content on the way scrolls by a row until it has been seen.
 */
static void form_move(struct fplinux_ui_screen *form, bool down)
{
	lv_obj_t *content = form->content;
	struct form_item *item = focused_item(form);
	lv_area_t view;
	lv_area_t area;
	size_t next;

	lv_obj_update_layout(content);
	view_area(content, &view);
	/* Read a wrapped action's hidden text before leaving its row. */
	if (item && item->kind == ITEM_VALUE) {
		lv_obj_t *label = reading_edge(item, down);

		if (label)
			lv_obj_get_coords(label, &area);
		if (label && (down ? area.y2 > view.y2 : area.y1 < view.y1)) {
			scroll_form(form, down ? -fplinux_ui_screen_spec(form)
							  ->row_height :
						 fplinux_ui_screen_spec(form)
							  ->row_height);
			return;
		}
	}
	if (item && inline_choice(item)) {
		size_t row = fplinux_ui_listview_selected(item->list);

		if (down ? row + 1 < item->count : row > 0) {
			fplinux_ui_listview_select(item->list,
						   down ? row + 1 : row - 1);
			item_area(item, &area);
			reveal(form, &area);
			return;
		}
	}
	next = neighbour(form, down, &view);
	if (next != NONE && !unread(form, down, next, &view)) {
		form_focus(form, next, down ? ENTER_TOP : ENTER_BOTTOM);
		return;
	}
	/* Read-only content scrolls by a row. */
	if (item)
		item_area(item, &area);
	scroll_form(form, down ? -fplinux_ui_screen_spec(form)->row_height :
				 fplinux_ui_screen_spec(form)->row_height);
	if (!item)
		return;
	/*
	 * A step that leaves the focused item, or a choice's focused row,
	 * partly hidden takes the focus from it, and the form reads on as one
	 * without focus; going back, the item takes it again once its edge is
	 * in sight (neighbour()). The form's padding still shows it.
	 */
	blur_scrolled_item(form, item, area.y1);
}

static void report_text(void *data);
static void popup_changed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context);

static void detach_popup(struct form_item *item)
{
	struct fplinux_ui_screen *screen = item->form->layer->transient;

	if (screen && screen->on_change == popup_changed &&
	    screen->context == item)
		fplinux_ui_screens_close_transient(item->form->ui, screen);
}

static void free_item(struct form_item *item)
{
	detach_popup(item);
	lv_async_call_cancel(report_text, item);
	if (item->choices)
		for (size_t i = 0; i < item->count; ++i)
			free(item->choices[i]);
	free(item->choices);
	free(item->content);
	free(item->reported);
	free(item->name);
	/* The image object, deleted before, owned item->image. */
	if (item->fitted)
		lv_draw_buf_destroy(item->fitted);
	free(item);
}

void fplinux_ui_form_free_items(struct fplinux_ui_screen *screen)
{
	for (size_t i = 0; i < screen->form.item_count; ++i)
		free_item(screen->form.items[i]);
	free(screen->form.items);
}

bool fplinux_ui_form_focus_is_actionable(const struct fplinux_ui_screen *screen)
{
	return actionable(focused_item(screen));
}

const char *fplinux_ui_form_action(const struct fplinux_ui_screen *screen)
{
	const struct form_item *item;

	if (screen->kind == SCREEN_TEXT_BOX)
		return fplinux_ui_text_takes_newline(
			       screen->form.items[0]->field) ?
			       fplinux_ui_screen_word(
				       screen, FPLINUX_UI_LABEL_NEW_LINE) :
			       NULL;
	item = focused_item(screen);
	if (!item)
		return NULL;
	if (segmented(item) || (item->kind == ITEM_CHOICE &&
				item->choice_kind == FPLINUX_UI_CHOICE_POPUP))
		return fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_EDIT);
	if (item->kind != ITEM_CHOICE)
		return NULL;
	if (item->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		return fplinux_ui_screen_mark_word(screen, item->list);
	return fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_SELECT);
}

/* Images */

static void image_parent_resized(lv_event_t *event);
static void form_images_resized(lv_event_t *event);
static bool show_image(struct form_item *item);

/* An image object owns the buffer it shows. */
static void image_delete_cb(lv_event_t *event)
{
	lv_draw_buf_destroy(lv_event_get_user_data(event));
}

/* Forms */

static bool is_form(const struct fplinux_ui_screen *screen)
{
	return screen && screen->kind == SCREEN_FORM;
}

static struct form_item *form_item(const struct fplinux_ui_screen *form,
				   size_t index, enum item_kind kind)
{
	if (!is_form(form) || index >= form->form.item_count ||
	    form->form.items[index]->kind != kind)
		return NULL;
	return form->form.items[index];
}

static bool starts_with_row(const struct form_item *item)
{
	return item->kind == ITEM_VALUE ||
	       (item->kind == ITEM_CHOICE && !item->label);
}

static bool readonly_property(const struct form_item *item)
{
	return item->property &&
	       item->appearance == FPLINUX_UI_APPEARANCE_PLAIN;
}

static void set_property_divider(struct form_item *item, bool divider)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(item->form);
	int32_t width = divider ? spec->divider_width : 0;
	int32_t padding = divider ? spec->gap_small : 0;

	if (item->divider == divider)
		return;
	item->divider = divider;
	lv_obj_set_style_pad_bottom(item->box, padding, 0);
	lv_obj_set_style_border_width(item->box, width, 0);
	lv_obj_set_style_border_side(item->box, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(
		item->box, fplinux_ui_theme(item->form->ui)->border, 0);
	lv_obj_set_style_border_opa(item->box, LV_OPA_COVER, 0);
}

/* Value rows reserve their divider so commands never change geometry. */
static void set_value_divider(struct form_item *item, bool divider)
{
	if (item->divider == divider)
		return;
	item->divider = divider;
	lv_obj_set_style_border_opa(item->box,
				    divider ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* Cells keep their sizes; spacing belongs to their visible boundaries. */
static void update_form_boundaries(struct fplinux_ui_screen *form)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(form);
	struct form_item *previous = NULL;
	int32_t top = 0, bottom = 0;

	for (size_t i = 0; i < form->form.item_count; ++i) {
		struct form_item *item = form->form.items[i];
		bool rows_meet;

		if (!item_visible(item)) {
			if (item->property)
				set_property_divider(item, false);
			if (item->kind == ITEM_VALUE)
				set_value_divider(item, false);
			continue;
		}
		rows_meet = previous &&
			    (previous->kind == ITEM_CHOICE ||
			     previous->kind == ITEM_VALUE) &&
			    starts_with_row(item);
		lv_obj_set_style_margin_top(
			item->box, !previous || rows_meet ? 0 : spec->spacing,
			0);
		if (!previous)
			top = starts_with_row(item) ? 0 : spec->content_padding;
		else if (previous->kind == ITEM_CHOICE)
			fplinux_ui_listview_set_trailing_divider(previous->list,
								 rows_meet);
		else if (previous->kind == ITEM_VALUE)
			set_value_divider(previous, rows_meet);
		else if (previous->property)
			set_property_divider(previous,
					     readonly_property(previous) &&
						     readonly_property(item));
		previous = item;
	}
	if (previous && previous->kind == ITEM_CHOICE) {
		fplinux_ui_listview_set_trailing_divider(previous->list, false);
		/* The line below the form supplies the final row's divider. */
		bottom = -spec->divider_width;
	} else if (previous) {
		if (previous->property)
			set_property_divider(previous, false);
		else if (previous->kind == ITEM_VALUE)
			set_value_divider(previous, false);
		bottom = spec->content_padding;
	}
	lv_obj_set_style_pad_top(form->content, top, 0);
	lv_obj_set_style_pad_bottom(form->content, bottom, 0);
}

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *ui,
					      const char *title)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_screen_new(ui, SCREEN_FORM, title);
	lv_obj_t *content;

	if (!form)
		return NULL;
	content = form->content;
	lv_obj_set_scrollable(content, true);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_ver(content, 0, 0);
	lv_obj_set_style_pad_row(content, 0, 0);
	fplinux_ui_screen_style_scrollbar(content, ui);
	lv_obj_add_event_cb(content, form_images_resized, LV_EVENT_SIZE_CHANGED,
			    form);
	return form;
}

static lv_obj_t *muted_label(struct fplinux_ui_screen *form, lv_obj_t *parent,
			     const char *text)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_color(label, fplinux_ui_theme(form->ui)->muted,
				    0);
	lv_label_set_text(label, text);
	return label;
}

static struct form_item *add_item(struct fplinux_ui_screen *form,
				  enum item_kind kind, const char *label)
{
	struct form_item *item;

	if (form->form.item_count == form->form.item_capacity) {
		size_t capacity = form->form.item_capacity ?
					  2 * form->form.item_capacity :
					  8;
		struct form_item **items =
			realloc(form->form.items, capacity * sizeof(*items));

		if (!items)
			return NULL;
		form->form.items = items;
		form->form.item_capacity = capacity;
	}
	item = calloc(1, sizeof(*item));
	if (!item)
		return NULL;
	item->form = form;
	item->index = form->form.item_count;
	item->kind = kind;
	item->box = fplinux_ui_screen_plain_object(form->content);
	lv_obj_set_size(item->box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(item->box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(item->box,
				 fplinux_ui_screen_spec(form)->gap_small, 0);
	lv_obj_set_style_pad_left(item->box,
				  fplinux_ui_screen_spec(form)->margin, 0);
	lv_obj_set_style_pad_right(item->box,
				   fplinux_ui_screen_spec(form)->margin, 0);
	if (label)
		item->label = muted_label(form, item->box, label);
	form->form.items[form->form.item_count++] = item;
	return item;
}

/* Undoes a failed append of the last item. */
static size_t drop_item(struct fplinux_ui_screen *form, int error)
{
	struct form_item *item = form->form.items[--form->form.item_count];

	lv_obj_delete(item->box);
	free_item(item);
	errno = error;
	return NONE;
}

static size_t appended(struct fplinux_ui_screen *form, struct form_item *item)
{
	update_form_boundaries(form);
	fplinux_ui_screen_changed(form);
	return item->index;
}

static void show_string(struct form_item *string);

static size_t append_string(struct fplinux_ui_screen *form, const char *label,
			    const char *text, bool property)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_STRING, label);
	if (!item)
		return NONE;
	item->property = property;
	if (property && item->label)
		lv_obj_set_style_text_color(
			item->label, fplinux_ui_theme(form->ui)->foreground, 0);
	item->value = lv_label_create(item->box);
	lv_obj_set_width(item->value, lv_pct(100));
	lv_label_set_long_mode(item->value, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(item->value, text ? text : "");
	show_string(item);
	return appended(form, item);
}

size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text)
{
	return append_string(form, label, text, false);
}

size_t fplinux_ui_form_append_property(struct fplinux_ui_screen *form,
				       const char *label, const char *text)
{
	return append_string(form, label, text, true);
}

/* Fit both complete strings on one line, otherwise wrap them vertically. */
static void layout_value(struct form_item *item)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(item->form);
	const lv_font_t *font = lv_obj_get_style_text_font(item->value, 0);
	const char *name = item->label ? lv_label_get_text(item->label) : "";
	const char *value = lv_label_get_text(item->value);
	int32_t letter_space =
		lv_obj_get_style_text_letter_space(item->value, 0);
	int32_t line_space = lv_obj_get_style_text_line_space(item->value, 0);
	lv_point_t name_size, value_size;
	int32_t width, height, content_height, value_y;
	bool inline_pair;

	if (item->value_layout)
		return;
	item->value_layout = true;
	width = lv_obj_get_content_width(item->box);
	if (width < 1) {
		item->value_layout = false;
		return;
	}
	lv_text_get_size(&name_size, name, font, letter_space, line_space,
			 LV_COORD_MAX, 0);
	lv_text_get_size(&value_size, value, font, letter_space, line_space,
			 LV_COORD_MAX, 0);
	if (!name[0])
		name_size = (lv_point_t){ 0, 0 };
	if (!value[0])
		value_size = (lv_point_t){ 0, 0 };
	lv_obj_set_hidden(item->value, !value[0]);
	inline_pair =
		!strchr(name, '\n') && !strchr(value, '\n') &&
		name_size.x + value_size.x +
				(name[0] && value[0] ? spec->row_gap : 0) <=
			width;
	if (inline_pair) {
		content_height = name_size.y > value_size.y ? name_size.y :
							      value_size.y;
		height = content_height + 2 * spec->field_padding +
			 spec->divider_width;
		if (height < spec->row_height)
			height = spec->row_height;
		value_y = (height - spec->divider_width - content_height) / 2;
		if (item->label) {
			lv_obj_set_width(item->label, name_size.x);
			lv_obj_set_pos(item->label, 0, value_y);
		}
		lv_obj_set_width(item->value, value_size.x);
		lv_obj_set_pos(item->value, width - value_size.x, value_y);
	} else {
		lv_text_get_size(&name_size, name, font, letter_space,
				 line_space, width, 0);
		lv_text_get_size(&value_size, value, font, letter_space,
				 line_space, width, 0);
		if (!value[0])
			value_size = (lv_point_t){ 0, 0 };
		value_y = spec->field_padding;
		if (item->label) {
			lv_obj_set_width(item->label, width);
			lv_obj_set_pos(item->label, 0, value_y);
			value_y +=
				name_size.y + (value[0] ? spec->gap_small : 0);
		}
		lv_obj_set_width(item->value, width);
		lv_obj_set_pos(item->value, 0, value_y);
		height = value_y + value_size.y + spec->field_padding +
			 spec->divider_width;
		if (height < spec->row_height)
			height = spec->row_height;
	}
	lv_obj_set_height(item->box, height);
	item->value_layout = false;
}

static void value_resized(lv_event_t *event)
{
	layout_value(lv_event_get_user_data(event));
}

size_t fplinux_ui_form_append_value(struct fplinux_ui_screen *form,
				    const char *label, const char *text)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_VALUE, label && label[0] ? label : NULL);
	if (!item)
		return NONE;
	lv_obj_set_layout(item->box, LV_LAYOUT_NONE);
	lv_obj_set_style_border_width(
		item->box, fplinux_ui_screen_spec(form)->divider_width, 0);
	lv_obj_set_style_border_side(item->box, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(item->box,
				      fplinux_ui_theme(form->ui)->border, 0);
	lv_obj_set_style_border_opa(item->box, LV_OPA_TRANSP, 0);
	item->value = lv_label_create(item->box);
	lv_label_set_long_mode(item->value, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(item->value, text ? text : "");
	lv_obj_add_event_cb(item->box, value_resized, LV_EVENT_SIZE_CHANGED,
			    item);
	lv_obj_update_layout(item->box);
	layout_value(item);
	return appended(form, item);
}

bool fplinux_ui_form_set_value_text(struct fplinux_ui_screen *form,
				    size_t index, const char *text)
{
	struct form_item *item = form_item(form, index, ITEM_VALUE);

	if (!item) {
		errno = EINVAL;
		return false;
	}
	lv_label_set_text(item->value, text ? text : "");
	layout_value(item);
	fplinux_ui_form_keep_focus_visible(form);
	return true;
}

/* An empty text takes no space, except in the box of a link or button. */
static void show_string(struct form_item *string)
{
	bool empty = !lv_label_get_text(string->value)[0] &&
		     !actionable(string);
	bool caption = string->label && lv_label_get_text(string->label)[0];

	lv_obj_set_hidden(string->value, empty);
	lv_obj_set_hidden(string->box, string->hidden || (empty && !caption));
}

/* A new text above the focus moves it: the form keeps it in view. */
bool fplinux_ui_form_set_string(struct fplinux_ui_screen *form, size_t item,
				const char *text)
{
	struct form_item *string = form_item(form, item, ITEM_STRING);
	bool visible;

	if (!string) {
		errno = EINVAL;
		return false;
	}
	visible = item_visible(string);
	lv_label_set_text(string->value, text ? text : "");
	show_string(string);
	if (item_visible(string) != visible)
		update_form_boundaries(form);
	/* A focused link or button keeps its new text in view too. */
	if (item <= form->form.focus)
		fplinux_ui_form_keep_focus_visible(form);
	return true;
}

/* Text items */

/* Reports a changed text once the text field's own callback is over. */
static void report_text(void *data)
{
	struct form_item *item = data;
	const char *text = fplinux_ui_text_get(item->field);
	char *copy;

	item->report_pending = false;
	if (!strcmp(text, item->reported) || !(copy = strdup(text)))
		return;
	free(item->reported);
	item->reported = copy;
	fplinux_ui_screen_report(item->form, item->index);
}

static void text_changed(struct fplinux_ui_text *field, void *context)
{
	struct form_item *item = context;

	fplinux_ui_screen_changed(item->form);
	if (item->quiet || item->report_pending ||
	    !strcmp(fplinux_ui_text_get(field), item->reported))
		return;
	if (lv_async_call(report_text, item) == LV_RESULT_OK)
		item->report_pending = true;
}

/*
 * Sets the text without reporting it as the user's change. The field may
 * store a DECIMAL separator of its own, which is one byte like the given one.
 */
static bool set_field_text(struct form_item *item, const char *text)
{
	size_t size = strlen(text) + 1;
	char *copy = malloc(size);
	bool done;

	if (!copy)
		return false;
	item->quiet = true;
	done = fplinux_ui_text_set(item->field, text);
	item->quiet = false;
	if (!done) {
		free(copy);
		return false;
	}
	snprintf(copy, size, "%s", fplinux_ui_text_get(item->field));
	free(item->reported);
	item->reported = copy;
	return true;
}

static bool make_field(struct form_item *item, lv_obj_t *parent, bool multiline,
		       size_t max_chars, const char *text)
{
	item->field = fplinux_ui_text_create(item->form->ui, parent, multiline,
					     max_chars);
	if (!item->field)
		return false;
	lv_obj_set_width(fplinux_ui_text_object(item->field), lv_pct(100));
	/* Unfocused first, so that a read-only field takes no marquee. */
	fplinux_ui_text_set_focused(item->field, false);
	if (!fplinux_ui_text_set_constraint(item->field, item->constraint))
		return false;
	if (!set_field_text(item, text ? text : ""))
		return false;
	fplinux_ui_text_set_change_cb(item->field, text_changed, item);
	return true;
}

size_t fplinux_ui_form_append_text_field(
	struct fplinux_ui_screen *form, const char *label, const char *text,
	size_t max_chars, enum fplinux_ui_text_constraint constraint)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_TEXT, label);
	if (!item)
		return NONE;
	item->constraint = constraint;
	if (!make_field(item, item->box, false, max_chars, text))
		return drop_item(form, errno);
	return appended(form, item);
}

/* A DECIMAL field, whatever its PASSWORD and UNEDITABLE modifiers. */
static bool is_decimal(const struct form_item *item)
{
	return ((unsigned int)item->constraint &
		~(unsigned int)(FPLINUX_UI_TEXT_PASSWORD |
				FPLINUX_UI_TEXT_UNEDITABLE)) ==
	       FPLINUX_UI_TEXT_DECIMAL;
}

/* Reads without replacing the borrowed result until the number changes. */
static const char *decimal_text(struct form_item *item)
{
	const char *text = fplinux_ui_text_get(item->field);
	const char *value = text;
	const char *cached = item->content;
	char *copy;
	char *separator;

	if (cached) {
		while (*value && *cached == (*value == ',' ? '.' : *value)) {
			++value;
			++cached;
		}
		if (!*value && !*cached)
			return item->content;
	}
	copy = strdup(text);
	if (!copy)
		return NULL;
	separator = strchr(copy, ',');
	if (separator)
		*separator = '.';
	free(item->content);
	item->content = copy;
	return copy;
}

/* A string item's text or a text field's text. */
const char *fplinux_ui_form_get_text(const struct fplinux_ui_screen *form,
				     size_t item)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);
	struct form_item *string = form_item(form, item, ITEM_STRING);

	if (field)
		return is_decimal(field) ? decimal_text(field) :
					   fplinux_ui_text_get(field->field);
	return string ? lv_label_get_text(string->value) : NULL;
}

bool fplinux_ui_form_set_text(struct fplinux_ui_screen *form, size_t item,
			      const char *text)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);

	if (!field || !text) {
		errno = EINVAL;
		return false;
	}
	if (!set_field_text(field, text))
		return false;
	fplinux_ui_screen_changed(form);
	return true;
}

/* Choices */

static void choice_selected(struct fplinux_ui_listview *list, size_t index,
			    void *context)
{
	struct form_item *item = context;

	(void)list;
	(void)index;
	fplinux_ui_screen_changed(item->form);
}

static bool copy_choices(struct form_item *item, const char *const *choices)
{
	item->choices = calloc(item->count, sizeof(*item->choices));
	if (!item->choices)
		return false;
	for (size_t i = 0; i < item->count; ++i)
		if (!(item->choices[i] = strdup(choices[i])))
			return false;
	return true;
}

/*
 * Rows of the list component without list padding; a popup choice is one
 * row with its complete value at the right, or on a second line.
 */
static bool make_choice_list(struct form_item *item, size_t rows,
			     enum fplinux_ui_listview_marker marker, bool popup)
{
	lv_obj_t *holder = fplinux_ui_screen_plain_object(item->box);

	lv_obj_set_size(holder, lv_pct(100),
			(int32_t)rows *
				fplinux_ui_screen_spec(item->form)->row_height);
	item->list = fplinux_ui_listview_create(item->form->ui, holder, marker);
	if (!item->list)
		return false;
	if (popup)
		fplinux_ui_listview_set_popup_choice(item->list);
	fplinux_ui_listview_set_selectable(item->list, false);
	fplinux_ui_listview_set_callbacks(item->list, choice_selected, NULL,
					  item);
	return true;
}

size_t fplinux_ui_form_append_choice(struct fplinux_ui_screen *form,
				     const char *label,
				     enum fplinux_ui_choice_kind kind,
				     const char *const *choices, size_t count)
{
	bool popup = kind == FPLINUX_UI_CHOICE_POPUP;
	struct form_item *item;

	if (!is_form(form) || (unsigned int)kind > FPLINUX_UI_CHOICE_POPUP ||
	    !choices || !count) {
		errno = EINVAL;
		return NONE;
	}
	for (size_t i = 0; i < count; ++i)
		if (!choices[i]) {
			errno = EINVAL;
			return NONE;
		}
	item = add_item(form, ITEM_CHOICE, popup ? NULL : label);
	if (!item)
		return NONE;
	item->choice_kind = kind;
	item->count = count;
	/* The rows bring their own insets; the label keeps the form's. */
	lv_obj_set_style_pad_hor(item->box, 0, 0);
	if (item->label) {
		lv_obj_set_style_pad_left(
			item->label, fplinux_ui_screen_spec(form)->margin, 0);
		lv_obj_set_style_pad_right(
			item->label, fplinux_ui_screen_spec(form)->margin, 0);
	}
	if ((popup && !copy_choices(item, choices)) ||
	    (popup && label && !(item->name = strdup(label))) ||
	    !make_choice_list(item, popup ? 1 : count,
			      kind == FPLINUX_UI_CHOICE_MULTIPLE ?
				      FPLINUX_UI_LISTVIEW_CHECK :
			      popup ? FPLINUX_UI_LISTVIEW_PLAIN :
				      FPLINUX_UI_LISTVIEW_RADIO,
			      popup))
		return drop_item(form, errno ? errno : ENOMEM);
	if (popup) {
		fplinux_ui_listview_add(item->list, label ? label : "",
					choices[0]);
	} else {
		for (size_t i = 0; i < count; ++i)
			fplinux_ui_listview_add(item->list, choices[i], NULL);
		if (kind == FPLINUX_UI_CHOICE_EXCLUSIVE)
			fplinux_ui_listview_set_marked(item->list, 0, true);
	}
	return appended(form, item);
}

static void set_chosen(struct form_item *item, size_t index)
{
	item->chosen = index;
	if (item->choice_kind == FPLINUX_UI_CHOICE_POPUP)
		fplinux_ui_listview_set_text(item->list, 0,
					     item->name ? item->name : "",
					     item->choices[index]);
	else
		fplinux_ui_listview_set_marked(item->list, index, true);
}

size_t fplinux_ui_form_get_choice(const struct fplinux_ui_screen *form,
				  size_t item)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		return NONE;
	return choice->chosen;
}

void fplinux_ui_form_set_choice(struct fplinux_ui_screen *form, size_t item,
				size_t index)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE ||
	    index >= choice->count)
		return;
	set_chosen(choice, index);
	fplinux_ui_screen_changed(form);
}

bool fplinux_ui_form_is_chosen(const struct fplinux_ui_screen *form,
			       size_t item, size_t index)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || index >= choice->count)
		return false;
	if (choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		return fplinux_ui_listview_is_marked(choice->list, index);
	return choice->chosen == index;
}

void fplinux_ui_form_set_chosen(struct fplinux_ui_screen *form, size_t item,
				size_t index, bool chosen)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || index >= choice->count)
		return;
	if (choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		fplinux_ui_listview_set_marked(choice->list, index, chosen);
	else if (chosen)
		set_chosen(choice, index);
	fplinux_ui_screen_changed(form);
}

/* The popup's list: Back leaves it, OK on a row chooses and returns. */
static void popup_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	(void)context;
	if (id == POPUP_BACK_COMMAND)
		fplinux_ui_screens_close_transient(ui, screen);
}

static void popup_changed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context)
{
	struct form_item *item = context;

	fplinux_ui_screens_close_transient(ui, screen);
	set_chosen(item, index);
	fplinux_ui_screen_changed(item->form);
	fplinux_ui_screen_report(item->form, item->index);
}

static void open_popup(struct fplinux_ui_screen *form, struct form_item *item)
{
	static const struct fplinux_ui_command back = {
		.id = POPUP_BACK_COMMAND,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		form->ui, item->name ? item->name : form->title,
		FPLINUX_UI_LIST_EXCLUSIVE);

	if (!list)
		return;
	fplinux_ui_screen_frame_popup_choice(list, item->count);
	for (size_t i = 0; i < item->count; ++i)
		fplinux_ui_listview_add(list->list, item->choices[i], NULL);
	fplinux_ui_listview_set_marked(list->list, item->chosen, true);
	fplinux_ui_listview_select(list->list, item->chosen);
	if (!fplinux_ui_screen_add_command(list, &back)) {
		fplinux_ui_screen_delete(list);
		return;
	}
	fplinux_ui_screen_set_listener(list, popup_command, popup_changed,
				       item);
	if (!fplinux_ui_screens_show_transient(form->ui, list, NULL, NULL))
		fplinux_ui_screen_delete(list);
}

/* Gauges */

static void show_number(struct form_item *gauge)
{
	lv_label_set_text_fmt(gauge->value, "%d", gauge->number);
	lv_bar_set_value(gauge->bar, gauge->number, LV_ANIM_OFF);
}

static lv_obj_t *make_frame(struct form_item *item, bool border)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(item->form);
	lv_obj_t *frame = fplinux_ui_screen_plain_object(item->box);

	lv_obj_set_size(frame, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(frame, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(frame, spec->frame_padding, 0);
	lv_obj_set_style_pad_row(frame, spec->gap_small, 0);
	lv_obj_set_style_bg_color(frame, theme->surface, 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(frame, spec->focus_width, 0);
	lv_obj_set_style_border_color(frame, theme->control_border, 0);
	lv_obj_set_style_border_opa(frame,
				    border ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	return frame;
}

size_t fplinux_ui_form_append_gauge(struct fplinux_ui_screen *form,
				    const char *label, int minimum, int maximum,
				    int value, bool interactive)
{
	const struct fplinux_ui_theme *theme;
	struct form_item *item;
	lv_obj_t *heading;

	if (!is_form(form) || minimum >= maximum) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_GAUGE, NULL);
	if (!item)
		return NONE;
	theme = fplinux_ui_theme(form->ui);
	item->minimum = minimum;
	item->maximum = maximum;
	item->number = value < minimum ? minimum :
		       value > maximum ? maximum :
					 value;
	item->interactive = interactive;
	item->frame = make_frame(item, interactive);
	if (!interactive) {
		lv_obj_set_style_pad_all(item->frame, 0, 0);
		lv_obj_set_style_bg_opa(item->frame, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(item->frame, 0, 0);
	}
	heading = fplinux_ui_screen_plain_object(item->frame);
	lv_obj_set_size(heading, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(heading,
				    fplinux_ui_screen_spec(form)->spacing, 0);
	item->gauge_name = lv_label_create(heading);
	lv_obj_set_flex_grow(item->gauge_name, 1);
	lv_obj_set_height(item->gauge_name,
			  interactive ? lv_font_get_line_height(theme->font) :
					LV_SIZE_CONTENT);
	lv_label_set_long_mode(item->gauge_name,
			       interactive ? LV_LABEL_LONG_MODE_DOTS :
					     LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(item->gauge_name, label ? label : "");
	item->value = lv_label_create(heading);
	lv_obj_set_style_text_color(item->value, theme->muted, 0);
	item->bar = fplinux_ui_screen_make_bar(item->frame, form->ui);
	lv_bar_set_range(item->bar, minimum, maximum);
	show_number(item);
	return appended(form, item);
}

int fplinux_ui_form_get_value(const struct fplinux_ui_screen *form, size_t item)
{
	struct form_item *gauge = form_item(form, item, ITEM_GAUGE);

	return gauge ? gauge->number : 0;
}

void fplinux_ui_form_set_value(struct fplinux_ui_screen *form, size_t item,
			       int value)
{
	struct form_item *gauge = form_item(form, item, ITEM_GAUGE);

	if (!gauge)
		return;
	gauge->number = value < gauge->minimum ? gauge->minimum :
			value > gauge->maximum ? gauge->maximum :
						 value;
	show_number(gauge);
}

/* One press moves a short range by 1 and a longer one by a twentieth. */
static void gauge_step(struct fplinux_ui_screen *form, struct form_item *gauge,
		       int direction)
{
	long range = (long)gauge->maximum - gauge->minimum;
	long step = range <= 20 ? 1 : range / 20;
	long number = gauge->number + direction * step;

	if (number < gauge->minimum)
		number = gauge->minimum;
	if (number > gauge->maximum)
		number = gauge->maximum;
	if (number == gauge->number)
		return;
	gauge->number = (int)number;
	show_number(gauge);
	fplinux_ui_screen_report(form, gauge->index);
}

/* Dates */

/*
 * The short date pattern that fplinux_ui_format_date() follows, with its
 * fallback to English, so that the editor shows the parts in the same order.
 * It is read again when the system language changes.
 */
static const char *date_pattern(struct fplinux_ui_screens *layer)
{
	const char *code = fplinux_ui_locale(layer->ui);
	struct fplinux_language language;

	if (layer->date_language[0] && !strcmp(layer->date_language, code))
		return layer->date_pattern;
	if (!fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, code,
				   &language) ||
	    !language.system)
		fplinux_language_english(&language);
	snprintf(layer->date_pattern, sizeof(layer->date_pattern), "%s",
		 language.date_short);
	snprintf(layer->date_language, sizeof(layer->date_language), "%s",
		 code);
	return layer->date_pattern;
}

static void begin_date_edit(struct form_item *item,
			    struct fplinux_ui_date_edit *edit)
{
	if (item->kind == ITEM_DURATION) {
		fplinux_ui_duration_edit_begin(edit, item->duration_seconds);
		return;
	}
	fplinux_ui_date_edit_begin(edit, item->date,
				   item->date_mode != FPLINUX_UI_TIME,
				   item->date_mode != FPLINUX_UI_DATE,
				   date_pattern(item->form->layer));
}

/* The value in the system language: short date, clock or both. */
static bool format_date_value(const struct form_item *item, char *text,
			      size_t size)
{
	struct fplinux_ui *ui = item->form->ui;
	size_t length;

	if (item->date_mode == FPLINUX_UI_TIME)
		return fplinux_ui_format_date(
			ui, item->date, FPLINUX_UI_DATE_CLOCK, text, size);
	if (!fplinux_ui_format_date(ui, item->date, FPLINUX_UI_DATE_SHORT, text,
				    size))
		return false;
	if (item->date_mode == FPLINUX_UI_DATE)
		return true;
	length = strlen(text);
	if (length + 1 >= size)
		return false;
	text[length++] = ' ';
	return fplinux_ui_format_date(ui, item->date, FPLINUX_UI_DATE_CLOCK,
				      text + length, size - length);
}

/*
 * The localized value, or while it is edited the editor's numbers in the
 * same order with the focused part selected.
 */
static void show_date(struct form_item *item,
		      const struct fplinux_ui_date_edit *edit)
{
	struct fplinux_ui_date_edit view;
	char text[DATE_VALUE_BYTES];
	size_t start = LV_DRAW_LABEL_NO_TXT_SEL;
	size_t end = LV_DRAW_LABEL_NO_TXT_SEL;

	if (edit) {
		fplinux_ui_date_edit_format(edit, text, sizeof(text), &start,
					    &end);
	} else if (item->kind == ITEM_DURATION ||
		   !format_date_value(item, text, sizeof(text))) {
		size_t unused;

		begin_date_edit(item, &view);
		fplinux_ui_date_edit_format(&view, text, sizeof(text), &unused,
					    &unused);
	}
	lv_label_set_text(item->value, text);
	lv_label_set_text_selection_start(item->value, (uint32_t)start);
	lv_label_set_text_selection_end(item->value, (uint32_t)end);
	fplinux_ui_marquee_changed(item->form->ui, item->value, NULL);
}

static void make_segmented_value(struct form_item *item)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);

	item->frame = make_frame(item, true);
	item->value = lv_label_create(item->frame);
	/* One line, so that a longer value ends with dots or scrolls. */
	lv_obj_set_size(item->value, lv_pct(100),
			lv_font_get_line_height(theme->font));
	lv_label_set_long_mode(item->value, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_bg_color(item->value, theme->accent, LV_PART_SELECTED);
	lv_obj_set_style_text_color(item->value, theme->accent_text,
				    LV_PART_SELECTED);
	show_date(item, NULL);
}

size_t fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
				   const char *label,
				   enum fplinux_ui_date_mode mode, time_t value)
{
	struct form_item *item;

	if (!is_form(form) || (unsigned int)mode > FPLINUX_UI_DATE_TIME) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_DATE, label);
	if (!item)
		return NONE;
	item->date_mode = mode;
	item->date = value;
	make_segmented_value(item);
	return appended(form, item);
}

size_t fplinux_ui_form_append_duration(struct fplinux_ui_screen *form,
				       const char *label, unsigned int seconds)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	if (seconds > FPLINUX_UI_DURATION_MAX_SECONDS) {
		errno = ERANGE;
		return NONE;
	}
	item = add_item(form, ITEM_DURATION, label);
	if (!item)
		return NONE;
	item->duration_seconds = seconds;
	make_segmented_value(item);
	return appended(form, item);
}

time_t fplinux_ui_form_get_date(const struct fplinux_ui_screen *form,
				size_t item)
{
	struct form_item *date = form_item(form, item, ITEM_DATE);

	return date ? date->date : (time_t)-1;
}

void fplinux_ui_form_set_date(struct fplinux_ui_screen *form, size_t item,
			      time_t value)
{
	struct form_item *date = form_item(form, item, ITEM_DATE);

	if (!date)
		return;
	date->date = value;
	if (!(form->form.editing && form->form.focus == item))
		show_date(date, NULL);
}

unsigned int fplinux_ui_form_get_duration(const struct fplinux_ui_screen *form,
					  size_t item)
{
	struct form_item *duration = form_item(form, item, ITEM_DURATION);

	if (!duration) {
		errno = EINVAL;
		return UINT_MAX;
	}
	return duration->duration_seconds;
}

bool fplinux_ui_form_set_duration(struct fplinux_ui_screen *form, size_t item,
				  unsigned int seconds)
{
	struct form_item *duration = form_item(form, item, ITEM_DURATION);

	if (!duration) {
		errno = EINVAL;
		return false;
	}
	if (seconds > FPLINUX_UI_DURATION_MAX_SECONDS) {
		errno = ERANGE;
		return false;
	}
	duration->duration_seconds = seconds;
	if (!(form->form.editing && form->form.focus == item))
		show_date(duration, NULL);
	return true;
}

/* Images */

/* Leave room for the measured caption and the item's optional frame. */
static void image_limits(const struct form_item *item, int32_t *width,
			 int32_t *height)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(item->form);
	int32_t inset =
		item->frame ? 2 * (spec->focus_width + spec->frame_padding) : 0;

	*width = lv_obj_get_content_width(item->box) - inset;
	*height = lv_obj_get_content_height(item->form->content) - inset;
	if (item->label)
		*height -= lv_obj_get_height(item->label) +
			   lv_obj_get_style_pad_row(item->box, 0);
	if (*width < 1)
		*width = 1;
	if (*height < 1)
		*height = 1;
}

size_t fplinux_ui_form_append_image(struct fplinux_ui_screen *form,
				    const char *label, const void *src)
{
	const struct fplinux_ui_theme *theme;
	struct form_item *item;
	lv_draw_buf_t *buffer;

	if (!is_form(form) || !src) {
		errno = EINVAL;
		return NONE;
	}
	theme = fplinux_ui_theme(form->ui);
	/* Keep the owned source so a larger view can restore its detail. */
	buffer = fplinux_ui_image_decode(src, INT_MAX, INT_MAX, false);
	if (!buffer)
		return NONE;
	item = add_item(form, ITEM_IMAGE, label);
	if (!item) {
		lv_draw_buf_destroy(buffer);
		return NONE;
	}
	lv_obj_set_flex_align(item->box, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	item->value = lv_image_create(item->box);
	lv_obj_set_style_max_width(item->value, lv_pct(100), 0);
	lv_image_set_inner_align(item->value, LV_IMAGE_ALIGN_CONTAIN);
	/*
	 * An alpha-only image (a glyph, a "-symbolic.png" file) is drawn in
	 * the form's text colour, as row icons are; without a recolour
	 * opacity a colour image is unchanged.
	 */
	lv_obj_set_style_image_recolor(item->value, theme->foreground, 0);
	lv_image_set_src(item->value, buffer);
	lv_obj_add_event_cb(item->value, image_delete_cb, LV_EVENT_DELETE,
			    buffer);
	item->image = buffer;
	lv_obj_add_event_cb(item->box, image_parent_resized,
			    LV_EVENT_SIZE_CHANGED, item);
	if (item->label)
		lv_obj_add_event_cb(item->label, image_parent_resized,
				    LV_EVENT_SIZE_CHANGED, item);
	lv_obj_update_layout(form->root);
	if (!show_image(item))
		return drop_item(form, ENOMEM);
	return appended(form, item);
}

static int key_digit(const struct fplinux_ui_key *key)
{
	if (key->source == FPLINUX_UI_KEY_PHONE)
		return key->code >= KEY_NUMERIC_0 &&
				       key->code <= KEY_NUMERIC_9 ?
			       (int)(key->code - KEY_NUMERIC_0) :
			       -1;
	return key->text[0] >= '0' && key->text[0] <= '9' && !key->text[1] ?
		       key->text[0] - '0' :
		       -1;
}

/*
 * While a segmented value is edited: Left and Right move between parts, Up adds 1 to
 * the part and Down subtracts 1, digits type, OK keeps the value and Cancel
 * restores it.
 */
void fplinux_ui_form_date_key(struct fplinux_ui_screen *form,
			      const struct fplinux_ui_key *key)
{
	struct form_item *item = form->form.items[form->form.focus];
	struct fplinux_ui_screens *layer = form->layer;
	size_t index = item->index;
	int digit = key_digit(key);
	bool changed = false;

	if (key->code == KEY_LEFT || key->code == KEY_RIGHT)
		fplinux_ui_date_edit_move(&form->form.edit,
					  key->code == KEY_LEFT ? -1 : 1);
	else if (key->code == KEY_UP || key->code == KEY_DOWN)
		fplinux_ui_date_edit_step(&form->form.edit,
					  key->code == KEY_UP ? 1 : -1);
	else if (digit >= 0)
		fplinux_ui_date_edit_digit(&form->form.edit,
					   (unsigned int)digit);
	else if (!fplinux_ui_screen_is_ok_key(key) &&
		 !fplinux_ui_screen_is_right_key(key))
		return;
	if (!fplinux_ui_screen_is_ok_key(key) &&
	    !fplinux_ui_screen_is_right_key(key)) {
		show_date(item, &form->form.edit);
		return;
	}
	form->form.editing = false;
	if (fplinux_ui_screen_is_ok_key(key)) {
		if (item->kind == ITEM_DURATION) {
			unsigned int seconds = fplinux_ui_duration_edit_value(
				&form->form.edit);

			changed = item->duration_seconds != seconds;
			item->duration_seconds = seconds;
		} else {
			time_t value =
				fplinux_ui_date_edit_value(&form->form.edit);

			changed = item->date != value;
			item->date = value;
		}
	}
	show_date(item, NULL);
	fplinux_ui_screen_refresh(layer);
	if (fplinux_ui_screen_is_ok_key(key) && form->form.on_edit)
		form->form.on_edit(form->ui, form, index,
				   form->form.edit_context);
	if (fplinux_ui_screen_is_alive(layer, form) && changed &&
	    index < form->form.item_count && form->form.items[index] == item)
		fplinux_ui_screen_report(form, index);
}

void fplinux_ui_form_set_edit_listener(struct fplinux_ui_screen *form,
				       fplinux_ui_change_cb accepted,
				       void *context)
{
	if (!is_form(form))
		return;
	form->form.on_edit = accepted;
	form->form.edit_context = context;
}

bool fplinux_ui_form_is_editing(const struct fplinux_ui_screen *form)
{
	return is_form(form) && form->form.editing;
}

void fplinux_ui_form_set_focus(struct fplinux_ui_screen *form, size_t item)
{
	if (!is_form(form) || item >= form->form.item_count ||
	    !focusable(form->form.items[item]) || form->form.editing)
		return;
	form->form.settled = true;
	form_focus(form, item, ENTER_KEEP);
	fplinux_ui_screen_changed(form);
}

/*
 * NONE until the form first shows an item that takes the focus or the
 * application sets it, and after the focus scrolled out of sight.
 */
size_t fplinux_ui_form_get_focus(const struct fplinux_ui_screen *form)
{
	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	return form->form.focus;
}

/* Item order: insert, delete, hide */

static bool focusable_at(size_t index, void *context)
{
	const struct fplinux_ui_screen *form = context;

	return focusable(form->form.items[index]);
}

/*
 * The focused item at from can no longer have the focus: the next item
 * that can takes it, else the previous one.
 */
static void pass_focus(struct fplinux_ui_screen *form, size_t from)
{
	size_t next = fplinux_ui_focus_next(from, form->form.item_count,
					    focusable_at, form);

	form->form.editing = false;
	form->form.focus = NONE;
	if (next != NONE)
		form_focus(form, next, ENTER_TOP);
	else
		fplinux_ui_form_apply_focus(form,
					    fplinux_ui_screen_is_top(form));
}

/* Items above the focus changed: the form scrolls to keep it in view. */
void fplinux_ui_form_keep_focus_visible(struct fplinux_ui_screen *form)
{
	lv_area_t area;

	if (form->form.focus == NONE)
		return;
	lv_obj_update_layout(form->content);
	item_area(form->form.items[form->form.focus], &area);
	reveal(form, &area);
}

/* Items after a change take the indices of their places. */
static void renumber(struct fplinux_ui_screen *form, size_t from)
{
	for (size_t i = from; i < form->form.item_count; ++i)
		form->form.items[i]->index = i;
}

/* Where an index goes when the item at from moves to to. */
static size_t moved_index(size_t index, size_t from, size_t to)
{
	if (index == from)
		return to;
	if (from < to && index > from && index <= to)
		return index - 1;
	if (to < from && index >= to && index < from)
		return index + 1;
	return index;
}

bool fplinux_ui_form_move_item(struct fplinux_ui_screen *form, size_t from,
			       size_t to)
{
	struct form_item *item;

	if (!is_form(form) || from >= form->form.item_count ||
	    to >= form->form.item_count) {
		errno = EINVAL;
		return false;
	}
	if (from == to)
		return true;
	item = form->form.items[from];
	if (from < to)
		memmove(&form->form.items[from], &form->form.items[from + 1],
			(to - from) * sizeof(*form->form.items));
	else
		memmove(&form->form.items[to + 1], &form->form.items[to],
			(from - to) * sizeof(*form->form.items));
	form->form.items[to] = item;
	lv_obj_move_to_index(item->box, (int32_t)to);
	renumber(form, from < to ? from : to);
	if (form->form.focus != NONE)
		form->form.focus = moved_index(form->form.focus, from, to);
	fplinux_ui_screen_item_commands_moved(form, moved_index, from, to);
	update_form_boundaries(form);
	fplinux_ui_form_keep_focus_visible(form);
	fplinux_ui_screen_changed(form);
	return true;
}

bool fplinux_ui_form_delete(struct fplinux_ui_screen *form, size_t item)
{
	struct form_item *deleted;
	bool focused;

	if (!is_form(form) || item >= form->form.item_count) {
		errno = EINVAL;
		return false;
	}
	deleted = form->form.items[item];
	focused = form->form.focus == item;
	detach_popup(deleted);
	--form->form.item_count;
	memmove(&form->form.items[item], &form->form.items[item + 1],
		(form->form.item_count - item) * sizeof(*form->form.items));
	lv_obj_delete(deleted->box);
	free_item(deleted);
	renumber(form, item);
	fplinux_ui_screen_item_commands_deleted(form, item, 1);
	update_form_boundaries(form);
	if (focused) {
		pass_focus(form, item);
	} else {
		if (form->form.focus != NONE && form->form.focus > item)
			--form->form.focus;
		fplinux_ui_form_keep_focus_visible(form);
	}
	fplinux_ui_screen_changed(form);
	return true;
}

void fplinux_ui_form_delete_all(struct fplinux_ui_screen *form)
{
	if (!is_form(form))
		return;
	for (size_t i = 0; i < form->form.item_count; ++i)
		detach_popup(form->form.items[i]);
	while (form->form.item_count) {
		struct form_item *item =
			form->form.items[form->form.item_count - 1];

		--form->form.item_count;
		lv_obj_delete(item->box);
		free_item(item);
	}
	fplinux_ui_screen_clear_item_commands(form);
	form->form.focus = NONE;
	form->form.editing = false;
	form->form.settled = false;
	update_form_boundaries(form);
	lv_obj_scroll_to_y(form->content, 0, LV_ANIM_OFF);
	fplinux_ui_screen_changed(form);
}

bool fplinux_ui_form_set_hidden(struct fplinux_ui_screen *form, size_t item,
				bool hidden)
{
	struct form_item *target;

	if (!is_form(form) || item >= form->form.item_count) {
		errno = EINVAL;
		return false;
	}
	target = form->form.items[item];
	if (target->hidden == hidden)
		return true;
	if (hidden && form->form.editing && form->form.focus == item)
		show_date(target, NULL);
	target->hidden = hidden;
	if (target->kind == ITEM_STRING)
		show_string(target);
	else
		lv_obj_set_hidden(target->box, hidden);
	update_form_boundaries(form);
	if (hidden && form->form.focus == item)
		pass_focus(form, item);
	else
		fplinux_ui_form_keep_focus_visible(form);
	fplinux_ui_screen_changed(form);
	return true;
}

bool fplinux_ui_form_is_hidden(const struct fplinux_ui_screen *form,
			       size_t item)
{
	return is_form(form) && item < form->form.item_count &&
	       form->form.items[item]->hidden;
}

bool fplinux_ui_form_set_constraint(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_text_constraint constraint)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);

	if (!field) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_text_set_constraint(field->field, constraint))
		return false;
	field->constraint = constraint;
	fplinux_ui_screen_changed(form);
	return true;
}

/* String and image items shown as links or buttons */

/*
 * The picture fits the actual item width, including a reserved scroll lane.
 * Bind a replacement before releasing its old buffer: setting a source can
 * run layout and deliver a parent-size callback again.
 */
static bool show_image(struct form_item *item)
{
	lv_draw_buf_t *previous = item->fitted;
	lv_draw_buf_t *fitted = NULL;
	const lv_draw_buf_t *buffer = item->image;
	int32_t width, height;
	uint32_t fitted_width, fitted_height;

	if (item->image_fitting)
		return true;
	item->image_fitting = true;
	image_limits(item, &width, &height);
	fplinux_ui_image_fit_size(&item->image->header, (uint32_t)width,
				  (uint32_t)height, &fitted_width,
				  &fitted_height);
	if (fitted_width != item->image->header.w ||
	    fitted_height != item->image->header.h) {
		if (previous && previous->header.w == fitted_width &&
		    previous->header.h == fitted_height)
			fitted = previous;
		else
			fitted = fplinux_ui_image_area_scale(
				item->image, fitted_width, fitted_height);
		if (!fitted) {
			/* Allocation failure must not expose an oversized source. */
			lv_obj_set_hidden(item->value, true);
			item->image_fitting = false;
			return false;
		}
		buffer = fitted;
	}
	item->fitted = fitted;
	lv_obj_set_hidden(item->value, false);
	/* A content-sized frame cannot size its child by percentages. */
	lv_obj_set_style_max_width(item->value, (int32_t)buffer->header.w, 0);
	if (lv_image_get_src(item->value) != buffer)
		lv_image_set_src(item->value, buffer);
	if (previous && previous != fitted)
		lv_draw_buf_destroy(previous);
	item->image_fitting = false;
	return true;
}

static void image_parent_resized(lv_event_t *event)
{
	struct form_item *item = lv_event_get_user_data(event);

	show_image(item);
}

static void form_images_resized(lv_event_t *event)
{
	struct fplinux_ui_screen *form = lv_event_get_user_data(event);

	for (size_t i = 0; i < form->form.item_count; ++i)
		if (form->form.items[i]->kind == ITEM_IMAGE &&
		    form->form.items[i]->image)
			show_image(form->form.items[i]);
}

/*
 * A link or button with item commands shows its value in a box with a
 * frame of focus_width and frame_padding inside. A
 * link has no visible frame: its text is underlined in the accent colour,
 * and an image has an accent line below it. A button is a surface box in a
 * border frame with its text centred. Without commands, or PLAIN, the item
 * is plain content again. Returns whether the item's size changed.
 */
static bool apply_appearance(struct form_item *item)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(item->form);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	bool link = item->appearance == FPLINUX_UI_APPEARANCE_HYPERLINK;
	bool image = item->kind == ITEM_IMAGE;
	bool created = false;
	lv_obj_t *frame;

	if (item->kind == ITEM_VALUE) {
		show_appearance_focus(
			item, actionable(item) &&
				      fplinux_ui_screen_is_top(item->form) &&
				      item->form->form.focus == item->index);
		return true;
	}
	if (!actionable(item)) {
		if ((item->kind != ITEM_STRING && !image) || !item->frame)
			return false;
		lv_obj_set_parent(item->value, item->box);
		lv_obj_delete(item->frame);
		item->frame = NULL;
		link = false;
	} else {
		if (!item->frame) {
			item->frame = fplinux_ui_screen_plain_object(item->box);
			lv_obj_set_parent(item->value, item->frame);
			created = true;
		}
		frame = item->frame;
		lv_obj_set_size(frame, image ? LV_SIZE_CONTENT : lv_pct(100),
				LV_SIZE_CONTENT);
		lv_obj_set_style_bg_color(
			frame, link ? theme->accent : theme->surface, 0);
		lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(frame, spec->focus_width, 0);
		lv_obj_set_style_border_color(
			frame, link ? theme->accent : theme->control_border, 0);
		lv_obj_set_style_border_opa(
			frame, link && !image ? LV_OPA_TRANSP : LV_OPA_COVER,
			0);
		/* The image's line takes a frame's place: the same size. */
		lv_obj_set_style_border_side(frame,
					     link && image ?
						     LV_BORDER_SIDE_BOTTOM :
						     LV_BORDER_SIDE_FULL,
					     0);
		lv_obj_set_style_pad_all(
			frame,
			spec->frame_padding +
				(link && image ? spec->focus_width : 0),
			0);
		lv_obj_set_style_pad_bottom(frame, spec->frame_padding, 0);
	}
	if (image) {
		show_image(item);
		lv_obj_set_style_image_recolor(
			item->value, link ? theme->accent : theme->foreground,
			0);
	} else {
		lv_obj_set_style_text_color(
			item->value, link ? theme->accent : theme->foreground,
			0);
		lv_obj_set_style_text_decor(
			item->value,
			link ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
		lv_obj_set_style_text_align(item->value,
					    item->frame && !link ?
						    LV_TEXT_ALIGN_CENTER :
						    LV_TEXT_ALIGN_AUTO,
					    0);
		show_string(item);
	}
	if (item->frame)
		show_appearance_focus(
			item, fplinux_ui_screen_is_top(item->form) &&
				      item->form->form.focus == item->index);
	return created || !item->frame;
}

/*
 * A form item's commands or appearance changed: a link or button looks
 * like one while it has item commands, and when it can no longer take the
 * focus, passes it on as a deleted item does.
 */
void fplinux_ui_form_update_appearance(struct fplinux_ui_screen *screen,
				       size_t index)
{
	struct form_item *item;
	bool resized;

	if (!is_form(screen))
		return;
	item = screen->form.items[index];
	resized = apply_appearance(item);
	update_form_boundaries(screen);
	if (!resized)
		return;
	if (screen->form.focus == index && !focusable(item))
		pass_focus(screen, index);
	else
		fplinux_ui_form_keep_focus_visible(screen);
}

bool fplinux_ui_form_set_appearance(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_appearance appearance)
{
	struct form_item *target;

	if (!is_form(form) || item >= form->form.item_count ||
	    (form->form.items[item]->kind != ITEM_STRING &&
	     form->form.items[item]->kind != ITEM_IMAGE) ||
	    (unsigned int)appearance > FPLINUX_UI_APPEARANCE_BUTTON) {
		errno = EINVAL;
		return false;
	}
	target = form->form.items[item];
	if (target->appearance == appearance)
		return true;
	target->appearance = appearance;
	fplinux_ui_form_update_appearance(form, item);
	fplinux_ui_screen_changed(form);
	return true;
}

/* OK on the focused item. */
void fplinux_ui_form_ok(struct fplinux_ui_screen *form)
{
	struct form_item *item = focused_item(form);
	size_t row;

	if (segmented(item)) {
		begin_date_edit(item, &form->form.edit);
		form->form.editing = true;
		show_date(item, &form->form.edit);
		return;
	}
	if (item->choice_kind == FPLINUX_UI_CHOICE_POPUP) {
		open_popup(form, item);
		return;
	}
	row = fplinux_ui_listview_selected(item->list);
	if (item->choice_kind == FPLINUX_UI_CHOICE_EXCLUSIVE)
		set_chosen(item, row);
	else
		fplinux_ui_listview_set_marked(
			item->list, row,
			!fplinux_ui_listview_is_marked(item->list, row));
	fplinux_ui_screen_report(form, item->index);
}

static bool form_page(struct fplinux_ui_screen *form,
		      const struct fplinux_ui_key *key)
{
	struct form_item *item = focused_item(form);
	lv_area_t area;

	if (item) {
		lv_obj_update_layout(form->content);
		item_area(item, &area);
	}
	if (!fplinux_ui_scroll_key(form->content, key))
		return false;
	if (key->pressed)
		settle_form_text_edge(form, key->code == KEY_RIGHT);
	if (item)
		blur_scrolled_item(form, item, area.y1);
	return true;
}

bool fplinux_ui_form_key(struct fplinux_ui_screen *form,
			 const struct fplinux_ui_key *key)
{
	struct form_item *item = focused_item(form);

	if (key->code == KEY_UP || key->code == KEY_DOWN) {
		form_move(form, key->code == KEY_DOWN);
		return true;
	}
	if (key->code == KEY_LEFT || key->code == KEY_RIGHT) {
		if (item && item->kind == ITEM_GAUGE) {
			gauge_step(form, item, key->code == KEY_LEFT ? -1 : 1);
			return true;
		}
		return form_page(form, key);
	}
	return false;
}

/* Text boxes */

struct fplinux_ui_screen *fplinux_ui_text_box_new(struct fplinux_ui *ui,
						  const char *title,
						  const char *text,
						  size_t max_chars)
{
	struct fplinux_ui_screen *box =
		fplinux_ui_screen_new(ui, SCREEN_TEXT_BOX, title);
	struct form_item *item;
	int error;

	if (!box)
		return NULL;
	box->form.items = calloc(1, sizeof(*box->form.items));
	item = calloc(1, sizeof(*item));
	if (!box->form.items || !item) {
		free(item);
		error = ENOMEM;
		goto fail;
	}
	box->form.items[0] = item;
	box->form.item_count = box->form.item_capacity = 1;
	box->form.focus = 0;
	item->form = box;
	item->kind = ITEM_TEXT;
	item->box = box->content;
	lv_obj_set_style_pad_all(
		box->content, fplinux_ui_screen_spec(box)->text_box_padding, 0);
	if (!make_field(item, box->content, true, max_chars, text)) {
		error = errno;
		goto fail;
	}
	lv_obj_set_height(fplinux_ui_text_object(item->field), lv_pct(100));
	return box;
fail:
	fplinux_ui_screen_delete(box);
	errno = error;
	return NULL;
}

const char *fplinux_ui_text_box_get(const struct fplinux_ui_screen *box)
{
	if (!box || box->kind != SCREEN_TEXT_BOX)
		return NULL;
	return fplinux_ui_text_get(box->form.items[0]->field);
}

bool fplinux_ui_text_box_set(struct fplinux_ui_screen *box, const char *text)
{
	if (!box || box->kind != SCREEN_TEXT_BOX || !text) {
		errno = EINVAL;
		return false;
	}
	if (!set_field_text(box->form.items[0], text))
		return false;
	fplinux_ui_screen_changed(box);
	return true;
}

bool fplinux_ui_text_box_cancel_picker(struct fplinux_ui_screen *box)
{
	if (!box || box->kind != SCREEN_TEXT_BOX) {
		errno = EINVAL;
		return false;
	}
	fplinux_ui_text_cancel_picker(box->form.items[0]->field);
	return true;
}

bool fplinux_ui_text_box_set_constraint(
	struct fplinux_ui_screen *box,
	enum fplinux_ui_text_constraint constraint)
{
	if (!box || box->kind != SCREEN_TEXT_BOX) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_text_set_constraint(box->form.items[0]->field,
					    constraint))
		return false;
	box->form.items[0]->constraint = constraint;
	fplinux_ui_screen_changed(box);
	return true;
}
