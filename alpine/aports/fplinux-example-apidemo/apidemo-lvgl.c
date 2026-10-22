/* SPDX-License-Identifier: 0BSD */
/*
 * LVGL widgets: every widget of the system LVGL, live on a Canvas and
 * operated with the phone keypad.
 *
 * How a screen of LVGL widgets works on FPLinux (the part to copy):
 *
 * - The widgets live in fplinux_ui_canvas_object() of a Canvas, in a
 *   scrolling container, so that tall content fits every screen profile.
 * - The runtime registers no LVGL input device: groups, focus and the
 *   keypad handling of the widgets get no keys by themselves. The screen
 *   creates its own keypad device (lv_indev_create(), LV_INDEV_TYPE_KEYPAD)
 *   in LV_INDEV_MODE_EVENT, so that no timer polls it and an idle screen
 *   does not wake the event loop, and binds it to an lv_group of the
 *   screen's interactive widgets. The canvas key handler stores each
 *   translated key and calls lv_indev_read(): the focused widget gets it as
 *   from any LVGL keypad.
 * - OK and the soft keys belong to commands. The OK command's label names
 *   what OK does on the focused widget (Press, Open ...); its listener
 *   gives LV_KEY_ENTER to the device. The left soft key, Next, moves the
 *   focus to the next widget of the group.
 * - LVGL's default theme gives each new widget its own colours, the
 *   Montserrat font and animated transitions. Every widget here drops them
 *   with lv_obj_remove_theme() and takes shared styles made from the
 *   FPLinux theme and metrics.
 * - Only the marquee and the busy spinner move on FPLinux: a focus move or
 *   a scroll happens at once. The widgets therefore scroll into view
 *   without LVGL's animated LV_OBJ_FLAG_SCROLL_ON_FOCUS, and the scroll
 *   animations that some widgets start on a key are finished at once.
 * - The device and the group are deleted with the screen (LV_EVENT_DELETE
 *   of the canvas object) and in the category's cleanup hook, which runs
 *   before fplinux_ui_close().
 *
 * The runtime pauses LVGL animations, such as the spinner and the animated
 * image, while the application is hidden, so this category needs no
 * visibility hook.
 */
#include "apidemo-lvgl.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The commands of a widget screen; Back is APIDEMO_COMMAND_BACK. */
enum {
	COMMAND_OK = 1,
	COMMAND_NEXT,
};

/* The one widget screen that exists, for the cleanup hook. */
static struct lvgl_demo *current;

/* Styles */

static void styles_init(struct styles *s, const struct fplinux_ui_theme *t,
			const struct fplinux_ui_metrics *m)
{
	lv_style_init(&s->note);
	lv_style_set_text_color(&s->note, t->muted);

	lv_style_init(&s->large);
	lv_style_set_text_font(&s->large, t->font_large);

	lv_style_init(&s->ring);
	lv_style_set_outline_width(&s->ring, m->focus_width);
	lv_style_set_outline_color(&s->ring, t->focus);
	lv_style_set_outline_pad(&s->ring, m->gap_small);

	lv_style_init(&s->frame);
	lv_style_set_bg_color(&s->frame, t->surface);
	lv_style_set_bg_opa(&s->frame, LV_OPA_COVER);
	lv_style_set_border_color(&s->frame, t->border);
	lv_style_set_border_width(&s->frame, m->focus_width);
	lv_style_set_pad_all(&s->frame, m->frame_padding);
	lv_style_set_text_color(&s->frame, t->foreground);

	lv_style_init(&s->frame_focused);
	lv_style_set_border_color(&s->frame_focused, t->focus);

	lv_style_init(&s->key);
	lv_style_set_bg_color(&s->key, t->surface);
	lv_style_set_bg_opa(&s->key, LV_OPA_COVER);
	lv_style_set_border_color(&s->key, t->border);
	lv_style_set_border_width(&s->key, m->focus_width);
	lv_style_set_pad_hor(&s->key, m->margin);
	lv_style_set_pad_ver(&s->key, m->gap_small);
	lv_style_set_pad_column(&s->key, m->spacing);
	lv_style_set_text_color(&s->key, t->foreground);
	lv_style_set_image_recolor(&s->key, t->foreground);
	lv_style_set_image_recolor_opa(&s->key, LV_OPA_COVER);
	lv_style_set_bg_image_recolor(&s->key, t->foreground);

	lv_style_init(&s->selected);
	lv_style_set_bg_color(&s->selected, t->accent);
	lv_style_set_bg_opa(&s->selected, LV_OPA_COVER);
	lv_style_set_border_color(&s->selected, t->accent);
	lv_style_set_text_color(&s->selected, t->accent_text);
	lv_style_set_image_recolor(&s->selected, t->accent_text);
	lv_style_set_bg_image_recolor(&s->selected, t->accent_text);

	lv_style_init(&s->disabled);
	lv_style_set_text_color(&s->disabled, t->disabled);
	lv_style_set_border_color(&s->disabled, t->disabled);
	lv_style_set_image_recolor(&s->disabled, t->disabled);
	lv_style_set_bg_image_recolor(&s->disabled, t->disabled);

	lv_style_init(&s->cell);
	lv_style_set_bg_color(&s->cell, t->surface);
	lv_style_set_bg_opa(&s->cell, LV_OPA_COVER);
	lv_style_set_border_color(&s->cell, t->border);
	lv_style_set_border_width(&s->cell, 1);
	lv_style_set_text_color(&s->cell, t->foreground);
	lv_style_set_pad_all(&s->cell, m->gap_small);

	lv_style_init(&s->row);
	lv_style_set_min_height(&s->row, m->row_height);
	lv_style_set_pad_hor(&s->row, m->margin);
	lv_style_set_pad_column(&s->row, m->spacing);
	lv_style_set_border_color(&s->row, t->border);
	lv_style_set_border_width(&s->row, 1);
	lv_style_set_border_side(&s->row, LV_BORDER_SIDE_BOTTOM);
	lv_style_set_text_color(&s->row, t->foreground);
	lv_style_set_image_recolor(&s->row, t->foreground);
	lv_style_set_image_recolor_opa(&s->row, LV_OPA_COVER);

	lv_style_init(&s->track);
	lv_style_set_bg_color(&s->track, t->border);
	lv_style_set_bg_opa(&s->track, LV_OPA_COVER);

	lv_style_init(&s->fill);
	lv_style_set_bg_color(&s->fill, t->focus);
	lv_style_set_bg_opa(&s->fill, LV_OPA_COVER);

	lv_style_init(&s->knob);
	lv_style_set_bg_color(&s->knob, t->focus);
	lv_style_set_bg_opa(&s->knob, LV_OPA_COVER);
	lv_style_set_radius(&s->knob, LV_RADIUS_CIRCLE);
	lv_style_set_pad_all(&s->knob, m->gap_small);

	lv_style_init(&s->arc_track);
	lv_style_set_arc_color(&s->arc_track, t->border);
	lv_style_set_arc_width(&s->arc_track, m->spacing);

	lv_style_init(&s->arc_fill);
	lv_style_set_arc_color(&s->arc_fill, t->focus);
	lv_style_set_arc_width(&s->arc_fill, m->spacing);

	lv_style_init(&s->scale_warning);
	lv_style_set_line_color(&s->scale_warning, t->warning);
	lv_style_set_arc_color(&s->scale_warning, t->warning);

	lv_style_init(&s->popup);
	lv_style_set_bg_color(&s->popup, t->popup);
	lv_style_set_bg_opa(&s->popup, LV_OPA_COVER);
	lv_style_set_border_color(&s->popup, t->focus);
	lv_style_set_border_width(&s->popup, m->focus_width);
	lv_style_set_text_color(&s->popup, t->foreground);
	lv_style_set_pad_all(&s->popup, m->margin);

	lv_style_init(&s->glyph);
	lv_style_set_image_recolor(&s->glyph, t->foreground);
	lv_style_set_image_recolor_opa(&s->glyph, LV_OPA_COVER);

	lv_style_init(&s->glyph_focus);
	lv_style_set_image_recolor(&s->glyph_focus, t->focus);
	lv_style_set_image_recolor_opa(&s->glyph_focus, LV_OPA_COVER);
}

static void styles_reset(struct styles *s)
{
	lv_style_t *all[] = {
		&s->note,	   &s->large,	      &s->ring,
		&s->frame,	   &s->frame_focused, &s->key,
		&s->selected,	   &s->disabled,      &s->cell,
		&s->row,	   &s->track,	      &s->fill,
		&s->knob,	   &s->arc_track,     &s->arc_fill,
		&s->popup,	   &s->glyph,	      &s->glyph_focus,
		&s->scale_warning,
	};

	for (size_t i = 0; i < APIDEMO_COUNT(all); i++)
		lv_style_reset(all[i]);
}

/*
 * Every new widget takes the styles of the display's theme: LVGL's default
 * theme with its own colours, font and transitions. Drop them from a widget
 * and from the parts it created, keeping what the widget set itself.
 */
void lvgl_untheme(lv_obj_t *object)
{
	uint32_t count = lv_obj_get_child_count(object);

	lv_obj_remove_theme(object, LV_PART_ANY | LV_STATE_ANY);
	for (uint32_t i = 0; i < count; i++)
		lvgl_untheme(lv_obj_get_child(object, i));
}

/* A button or key: framed, accent while focused, dimmed when disabled. */
void lvgl_style_key(struct lvgl_demo *demo, lv_obj_t *object,
		    lv_style_selector_t part)
{
	lv_obj_add_style(object, &demo->styles.key, part);
	lv_obj_add_style(object, &demo->styles.selected,
			 part | LV_STATE_FOCUSED);
	lv_obj_add_style(object, &demo->styles.disabled,
			 part | LV_STATE_DISABLED);
}

/* A framed control whose frame turns to the focus colour. */
void lvgl_style_frame(struct lvgl_demo *demo, lv_obj_t *object)
{
	lv_obj_add_style(object, &demo->styles.frame, 0);
	lv_obj_add_style(object, &demo->styles.frame_focused, LV_STATE_FOCUSED);
	lv_obj_add_style(object, &demo->styles.disabled, LV_STATE_DISABLED);
}

/* The focus ring of a control without a frame. */
void lvgl_style_ring(struct lvgl_demo *demo, lv_obj_t *object)
{
	lv_obj_add_style(object, &demo->styles.ring, LV_STATE_FOCUSED);
}

/* A native popup and its scrim leave together, including parent teardown. */
static void scrim_deleted(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);

	demo->scrim = NULL;
}

static void popup_deleted(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);

	demo->popup = NULL;
	if (demo->scrim)
		lv_obj_delete(demo->scrim);
}

static void popup_hide(struct lvgl_demo *demo)
{
	if (demo->popup)
		lv_obj_remove_event_cb_with_user_data(demo->popup,
						      popup_deleted, demo);
	demo->popup = NULL;
	if (demo->scrim)
		lv_obj_delete(demo->scrim);
}

/* Only the background is translucent: popup text stays fully opaque. */
void lvgl_popup_show(struct lvgl_demo *demo, lv_obj_t *popup)
{
	lv_obj_t *root = fplinux_ui_screen(demo->app->ui);
	lv_obj_t *scrim;

	if (demo->popup == popup)
		return;
	/* A closed message box may still await its timer-turn deletion. Its
	 * closing callback has returned before another popup can open: delete
	 * it now so no styled sibling outlives this demo. LVGL cancels queued
	 * deletion when the object is deleted synchronously. Dropdown lists,
	 * in contrast, stay owned by their widget and are only hidden. */
	if (demo->popup && lv_obj_check_type(demo->popup, &lv_msgbox_class))
		lv_obj_delete(demo->popup);
	else
		popup_hide(demo);
	/* A canvas child must rise above a scrim that also covers its title. */
	if (lv_obj_get_parent(popup) != root) {
		lv_area_t box;

		lv_obj_update_layout(popup);
		lv_obj_get_coords(popup, &box);
		lv_obj_set_parent(popup, root);
		lv_obj_set_align(popup, LV_ALIGN_TOP_LEFT);
		lv_obj_set_pos(popup, box.x1, box.y1);
	}
	scrim = lvgl_box_new(root);
	lv_obj_set_size(scrim, fplinux_ui_width(demo->app->ui),
			fplinux_ui_height(demo->app->ui));
	lv_obj_set_style_bg_color(scrim, theme(demo)->scrim, 0);
	lv_obj_set_style_bg_opa(scrim, theme(demo)->scrim_opacity, 0);
	lv_obj_move_to_index(popup, -1);
	demo->popup = popup;
	demo->scrim = scrim;
	lv_obj_add_event_cb(scrim, scrim_deleted, LV_EVENT_DELETE, demo);
	lv_obj_add_event_cb(popup, popup_deleted, LV_EVENT_DELETE, demo);
}

/* LVGL retains its dropdown list between opens: the scrim does not linger. */
static void dropdown_popup(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);
	lv_obj_t *list = lv_dropdown_get_list(lv_event_get_target_obj(event));

	if (lv_event_get_code(event) == LV_EVENT_READY)
		lvgl_popup_show(demo, list);
	else if (demo->popup == list)
		popup_hide(demo);
}

/* A dropdown's list: a popup layer with the selected option in accent. */
void lvgl_style_dropdown_list(struct lvgl_demo *demo, lv_obj_t *dropdown)
{
	lv_obj_t *list = lv_dropdown_get_list(dropdown);

	lvgl_untheme(list);
	lv_obj_add_style(list, &demo->styles.popup, 0);
	lv_obj_add_style(list, &demo->styles.selected,
			 LV_PART_SELECTED | LV_STATE_CHECKED);
	lv_obj_add_style(list, &demo->styles.selected,
			 LV_PART_SELECTED | LV_STATE_PRESSED);
	lv_obj_set_style_text_line_space(list, metrics(demo)->gap_small * 2, 0);
	lv_obj_set_style_max_height(list, area_height(demo), 0);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(dropdown, dropdown_popup, LV_EVENT_READY, demo);
	lv_obj_add_event_cb(dropdown, dropdown_popup, LV_EVENT_CANCEL, demo);
}

/* Objects */

/* A plain, transparent container without the theme's styles. */
lv_obj_t *lvgl_box_new(lv_obj_t *parent)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_remove_style_all(box);
	lv_obj_set_scrollable(box, false);
	return box;
}

/* A full-width row of the page that lays its children out side by side. */
lv_obj_t *lvgl_row_new(struct lvgl_demo *demo, lv_obj_t *parent)
{
	lv_obj_t *row = lvgl_box_new(parent);

	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(row, metrics(demo)->spacing, 0);
	lv_obj_set_style_pad_row(row, metrics(demo)->spacing, 0);
	/*
	 * The children's focus rings reach beyond the row. In LVGL 9.6,
	 * OVERFLOW_VISIBLE clips at the parent's extended draw box, so both
	 * the flag and room in that box are needed.
	 */
	lv_obj_set_overflow_visible(row, true);
	lv_obj_set_ext_draw_size(row, metrics(demo)->focus_width +
					      metrics(demo)->gap_small);
	return row;
}

/* A column of a row, width wide, for a caption over a widget. */
lv_obj_t *lvgl_column_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  int32_t width)
{
	lv_obj_t *column = lvgl_box_new(parent);

	lv_obj_set_size(column, width, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(column, metrics(demo)->gap_small, 0);
	lv_obj_set_overflow_visible(column, true);
	lv_obj_set_ext_draw_size(column, metrics(demo)->focus_width +
						 metrics(demo)->gap_small);
	return column;
}

/* A one-line label: one line high, ending with "…" when it does not fit. */
lv_obj_t *lvgl_line_new(lv_obj_t *parent, const char *text,
			const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_remove_theme(label, LV_PART_ANY | LV_STATE_ANY);
	lv_obj_set_style_text_font(label, font, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_size(label, lv_pct(100), lv_font_get_line_height(font));
	lv_label_set_text(label, text);
	return label;
}

/* A caption in the secondary colour, such as the name of a variant. */
lv_obj_t *lvgl_caption_new(struct lvgl_demo *demo, lv_obj_t *parent,
			   const char *text)
{
	lv_obj_t *label = lvgl_line_new(parent, text, theme(demo)->font);

	lv_obj_add_style(label, &demo->styles.note, 0);
	return label;
}

/* A paragraph that wraps. */
lv_obj_t *lvgl_paragraph_new(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_remove_theme(label, LV_PART_ANY | LV_STATE_ANY);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_text(label, text);
	return label;
}

/* The key note at the end of the page: which keys do what. */
lv_obj_t *lvgl_note_new(struct lvgl_demo *demo, lv_obj_t *parent,
			const char *text)
{
	lv_obj_t *label = lvgl_paragraph_new(parent, text);

	lv_obj_add_style(label, &demo->styles.note, 0);
	demo->key_note = label;
	return label;
}

/* The line that reports what the widgets did. */
lv_obj_t *lvgl_status_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  const char *text)
{
	demo->status = lvgl_line_new(parent, text, theme(demo)->font);
	return demo->status;
}

lv_obj_t *lvgl_button_new(struct lvgl_demo *demo, lv_obj_t *parent,
			  const char *text)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_t *label = lv_label_create(button);

	lvgl_untheme(button);
	lvgl_style_key(demo, button, LV_PART_MAIN);
	lv_obj_set_height(button, metrics(demo)->row_height);
	lv_label_set_text(label, text);
	lv_obj_center(label);
	return button;
}

/*
 * A row of the page that reaches both edges, its content inset by margin
 * and gutter like a list row: negative margins, which the flex layout
 * honours, undo the page's padding.
 */
void lvgl_full_width(struct lvgl_demo *demo, lv_obj_t *row)
{
	const struct fplinux_ui_metrics *m = metrics(demo);

	lv_obj_set_width(row, fplinux_ui_width(demo->app->ui));
	lv_obj_set_style_margin_left(row, -m->margin, 0);
	lv_obj_set_style_margin_right(row, -m->gutter, 0);
	lv_obj_set_style_pad_left(row, m->margin, 0);
	lv_obj_set_style_pad_right(row, m->gutter, 0);
}

/* Keeps an image buffer until the screen goes; NULL stays NULL. */
lv_draw_buf_t *lvgl_keep_buffer(struct lvgl_demo *demo, lv_draw_buf_t *buffer)
{
	if (!buffer)
		return NULL;
	if (demo->buffer_count == BUFFER_LIMIT) {
		lv_draw_buf_destroy(buffer);
		return NULL;
	}
	demo->buffers[demo->buffer_count++] = buffer;
	return buffer;
}

/* Focus and the OK command */

/* The focused widget's entry, or NULL while nothing shows the focus. */
static struct focusable *focused_entry(struct lvgl_demo *demo)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	if (demo->unfocused)
		return NULL;
	for (size_t i = 0; i < demo->focusable_count; i++)
		if (demo->focusables[i].object == focused)
			return &demo->focusables[i];
	return NULL;
}

static const char *ok_label(struct lvgl_demo *demo, const struct focusable *f)
{
	switch (f->ok) {
	case OK_PRESS:
		return _("Press");
	case OK_TOGGLE:
		return _("Toggle");
	case OK_MARK:
		return lv_obj_is_checked(f->object) ? _("Unmark") : _("Mark");
	case OK_OPEN:
		if (lv_obj_check_type(f->object, &lv_dropdown_class) &&
		    lv_dropdown_is_open(f->object))
			return _("Select");
		return _("Open");
	case OK_SELECT:
		return _("Select");
	case OK_DONE:
		return _("Done");
	case OK_NEW_LINE:
		return _("New line");
	case OK_PLAY:
		return demo->u.animimg.running ? _("Pause") : _("Play");
	case OK_RESET:
		return _("Reset");
	case OK_NONE:
	default:
		return NULL;
	}
}

/*
 * The centre legend names what OK does on the focused widget. Adding a
 * command with the same id relabels it; without an action OK has none.
 */
void lvgl_update_ok(struct lvgl_demo *demo)
{
	const struct focusable *f = focused_entry(demo);
	const char *label = f ? ok_label(demo, f) : NULL;
	struct fplinux_ui_command ok = {
		.id = COMMAND_OK,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = label,
	};

	if (label == demo->ok_shown)
		return;
	if (label)
		fplinux_ui_screen_add_command(demo->screen, &ok);
	else
		fplinux_ui_screen_remove_command(demo->screen, COMMAND_OK);
	demo->ok_shown = label;
}

/* Whether object lies in the page, which scrolls, rather than beside it. */
static bool on_page(struct lvgl_demo *demo, lv_obj_t *object)
{
	for (; object && demo->page; object = lv_obj_get_parent(object))
		if (object == demo->page)
			return true;
	return false;
}

/*
 * Whether object shows in the page's view: in part, or whole as a focused
 * widget must. A widget taller than the view counts as whole while it
 * fills the view.
 */
static bool in_view(struct lvgl_demo *demo, lv_obj_t *object, bool whole)
{
	lv_area_t view;
	lv_area_t box;

	if (!on_page(demo, object))
		return true;
	lv_obj_get_coords(demo->page, &view);
	lv_obj_get_coords(object, &box);
	if (!whole || lv_area_get_height(&box) > lv_area_get_height(&view))
		return box.y1 <= view.y2 && box.y2 >= view.y1;
	return box.y1 >= view.y1 && box.y2 <= view.y2;
}

/*
 * LVGL calls this when the focus moves. Scroll the widget into view at
 * once: the widgets' own LV_OBJ_FLAG_SCROLL_ON_FOCUS would animate. A
 * widget taller than the view shows its top.
 */
static void focus_changed(lv_group_t *group)
{
	struct lvgl_demo *demo = lv_group_get_user_data(group);
	lv_obj_t *focused = lv_group_get_focused(group);
	struct focusable *f;
	int32_t ring;
	lv_area_t box;
	lv_area_t top;
	int32_t view;

	if (!demo || !focused)
		return;
	demo->unfocused = false;
	f = focused_entry(demo);
	lv_obj_update_layout(focused);
	/* Beside the page, such as in a message box, it is always seen. */
	if (on_page(demo, focused)) {
		/* With its focus ring, and what goes above it if both fit. */
		ring = metrics(demo)->focus_width + metrics(demo)->gap_small;
		lv_obj_get_coords(focused, &box);
		/*
		 * What goes above it too, unless only the widget would fit:
		 * a widget taller than the view shows from that top.
		 */
		if (f && f->top) {
			lv_obj_get_coords(f->top, &top);
			view = lv_obj_get_content_height(demo->page);
			if (box.y2 - top.y1 + ring <= view ||
			    lv_area_get_height(&box) > view)
				box.y1 = LV_MIN(box.y1, top.y1 + ring);
		}
		lvgl_show_band(demo, box.y1 - ring, box.y2 + ring);
	}
	lvgl_update_ok(demo);
}

/* The focus comes back to object, or moves there. */
static void focus_object(struct lvgl_demo *demo, lv_obj_t *object)
{
	if (demo->unfocused && object == lv_group_get_focused(demo->group)) {
		/* LVGL kept the focus there: show it again. */
		lv_obj_set_focused(object, true);
		lv_obj_set_focus_key(object, true);
		focus_changed(demo->group);
		return;
	}
	lv_group_focus_obj(object);
}

/*
 * After a scroll, as on the system's forms: a focused widget scrolled out
 * of view gives up the focus, and takes it again once back in view.
 */
static void scrolled(struct lvgl_demo *demo)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	if (!focused || !demo->page)
		return;
	lv_obj_update_layout(demo->page);
	if (demo->unfocused && in_view(demo, focused, true)) {
		focus_object(demo, focused);
	} else if (!demo->unfocused && !in_view(demo, focused, true)) {
		demo->unfocused = true;
		lv_obj_set_focused(focused, false);
		lv_obj_set_focus_key(focused, false);
		lvgl_update_ok(demo);
	}
}

/*
 * Up and Down on a widget that does not take them work as on the system's
 * forms: the focus moves to the widget above or below when it is in view;
 * otherwise the page scrolls by a row.
 */
static void page_move(struct lvgl_demo *demo, bool down)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);
	size_t count = demo->focusable_count;
	size_t index = count;

	if (demo->page)
		lv_obj_update_layout(demo->page);
	for (size_t i = 0; i < count; i++)
		if (demo->focusables[i].object == focused)
			index = i;
	if (index < count && (down ? index + 1 < count : index > 0)) {
		lv_obj_t *next =
			demo->focusables[down ? index + 1 : index - 1].object;

		if (in_view(demo, next, false)) {
			lv_group_focus_obj(next);
			return;
		}
	}
	if (!demo->page)
		return;
	lv_obj_scroll_by_bounded(demo->page, 0,
				 down ? -metrics(demo)->row_height :
					metrics(demo)->row_height,
				 LV_ANIM_OFF);
	scrolled(demo);
}

/* Adds an interactive widget to the group, in focus order. */
void lvgl_add_focusable(struct lvgl_demo *demo, lv_obj_t *object,
			enum ok_action ok, unsigned int uses)
{
	if (demo->focusable_count == FOCUSABLE_LIMIT)
		return;
	demo->focusables[demo->focusable_count++] = (struct focusable){
		.object = object,
		.top = NULL,
		.ok = ok,
		.uses = uses,
	};
	lv_obj_set_scroll_on_focus(object, false);
	lv_group_add_obj(demo->group, object);
}

void lvgl_show_with(struct lvgl_demo *demo, lv_obj_t *object, lv_obj_t *top)
{
	for (size_t i = 0; i < demo->focusable_count; i++)
		if (demo->focusables[i].object == object)
			demo->focusables[i].top = top;
}

/* Empties the group, for a screen whose widgets change. */
void lvgl_clear_focusables(struct lvgl_demo *demo)
{
	lv_group_remove_all_objs(demo->group);
	demo->focusable_count = 0;
}

/*
 * Some widgets scroll with an animation on a key, such as an open
 * dropdown's list. Jump to where the animation ends, as the runtime's own
 * screens do.
 */
static void finish_animations(lv_obj_t *object)
{
	lv_anim_t *animation;

	while ((animation = lv_anim_get(object, NULL))) {
		lv_anim_exec_xcb_t exec = animation->exec_cb;
		int32_t end = animation->end_value;

		lv_anim_delete(object, exec);
		if (exec)
			exec(object, end);
	}
}

/*
 * LVGL opens a dropdown's list below the field or, short of room, above
 * it, measured on the whole display, so that it may cover the title band.
 * Keep it in the canvas object: the field goes to the top of the view and
 * the list where the most room is.
 */
static void place_dropdown_list(struct lvgl_demo *demo, lv_obj_t *dropdown)
{
	lv_obj_t *list = lv_dropdown_get_list(dropdown);
	lv_area_t area;
	lv_area_t field;
	int32_t height;
	int32_t below;
	int32_t above;

	if (!lv_dropdown_is_open(dropdown)) {
		demo->list_placed = false;
		return;
	}
	if (demo->list_placed)
		return;
	demo->list_placed = true;
	lv_obj_update_layout(dropdown);
	lv_obj_get_coords(dropdown, &field);
	lvgl_show_band(demo, field.y1, LV_COORD_MAX / 2);
	lv_obj_update_layout(dropdown);
	lv_obj_get_coords(dropdown, &field);
	lv_obj_get_coords(demo->area, &area);
	lv_obj_set_height(list, LV_SIZE_CONTENT);
	lv_obj_update_layout(list);
	height = lv_obj_get_height(list);
	below = area.y2 - field.y2;
	above = field.y1 - area.y1;
	/*
	 * Over the field, as a pop-up of its options, downwards or upwards;
	 * the list's parent is the LVGL screen: screen coordinates.
	 */
	if (height <= below + lv_area_get_height(&field) || below >= above) {
		height = LV_MIN(height, area.y2 - field.y1 + 1);
		lv_obj_set_y(list, field.y1);
	} else {
		height = LV_MIN(height, field.y2 - area.y1 + 1);
		lv_obj_set_y(list, field.y2 + 1 - height);
	}
	lv_obj_set_height(list, height);
}

/* Keeps the selected key of a matrix or a keyboard visible on a small page. */
static void show_selected_key(struct lvgl_demo *demo, lv_obj_t *matrix)
{
	const char *const *map = lv_buttonmatrix_get_map(matrix);
	uint32_t selected = lv_buttonmatrix_get_selected_button(matrix);
	uint32_t button = 0, row = 0, selected_row = 0, rows = 1;

	if (selected == LV_BUTTONMATRIX_BUTTON_NONE || !map)
		return;
	for (size_t i = 0; map[i][0]; i++) {
		if (!strcmp(map[i], "\n")) {
			row++;
			rows++;
		} else {
			if (button++ == selected)
				selected_row = row;
		}
	}
	lvgl_show_row(demo, matrix, selected_row, rows);
}

/* After a key: no transition left, and the centre legend up to date. */
static void settle(struct lvgl_demo *demo)
{
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	for (lv_obj_t *object = focused; object && object != demo->area;
	     object = lv_obj_get_parent(object)) {
		/* The animated image is functional motion, not a transition. */
		if (!lv_obj_check_type(object, &lv_animimg_class))
			finish_animations(object);
	}
	if (focused && lv_obj_check_type(focused, &lv_dropdown_class)) {
		place_dropdown_list(demo, focused);
		finish_animations(lv_dropdown_get_list(focused));
	} else if (focused &&
		   (lv_obj_check_type(focused, &lv_buttonmatrix_class) ||
		    lv_obj_check_type(focused, &lv_keyboard_class))) {
		show_selected_key(demo, focused);
	}
	if (demo->page)
		finish_animations(demo->page);
	lvgl_update_ok(demo);
}

/*
 * Scrolls the page so that the band from top to bottom (screen
 * coordinates) is visible, for a part of a widget such as a table row.
 */
void lvgl_show_band(struct lvgl_demo *demo, int32_t top, int32_t bottom)
{
	lv_area_t view;
	int32_t delta = 0;

	if (!demo->page)
		return;
	lv_obj_get_content_coords(demo->page, &view);
	if (bottom > view.y2)
		delta = bottom - view.y2;
	if (top - delta < view.y1)
		delta = top - view.y1;
	if (delta)
		lv_obj_scroll_by_bounded(demo->page, 0, -delta, LV_ANIM_OFF);
}

/* Shows row index of rows equal rows of object, such as a matrix's keys. */
void lvgl_show_row(struct lvgl_demo *demo, lv_obj_t *object, uint32_t row,
		   uint32_t rows)
{
	lv_area_t box;
	int32_t height;

	lv_obj_update_layout(object);
	lv_obj_get_content_coords(object, &box);
	height = lv_area_get_height(&box);
	lvgl_show_band(demo, box.y1 + height * (int32_t)row / (int32_t)rows,
		       box.y1 + height * (int32_t)(row + 1) / (int32_t)rows);
}

/* The keypad device */

/* LVGL reads the device: the key that the key handler stored. */
static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct lvgl_demo *demo = lv_indev_get_user_data(indev);

	data->key = demo->read_key;
	data->state = demo->read_state;
}

static bool keypad_new(struct lvgl_demo *demo)
{
	demo->group = lv_group_create();
	demo->keypad = lv_indev_create();
	if (!demo->group || !demo->keypad)
		return false;
	lv_group_set_user_data(demo->group, demo);
	lv_group_set_focus_cb(demo->group, focus_changed);
	lv_indev_set_type(demo->keypad, LV_INDEV_TYPE_KEYPAD);
	/* Read only when the key handler calls lv_indev_read(): no timer. */
	lv_indev_set_mode(demo->keypad, LV_INDEV_MODE_EVENT);
	lv_indev_set_read_cb(demo->keypad, keypad_read);
	lv_indev_set_user_data(demo->keypad, demo);
	lv_indev_set_group(demo->keypad, demo->group);
	return true;
}

static void keypad_delete(struct lvgl_demo *demo)
{
	if (demo->keypad)
		lv_indev_delete(demo->keypad);
	if (demo->group)
		lv_group_delete(demo->group);
	demo->keypad = NULL;
	demo->group = NULL;
}

/* One key change for LVGL: the group's focused widget receives it. */
void lvgl_keypad_feed(struct lvgl_demo *demo, uint32_t key, bool pressed)
{
	demo->read_key = key;
	demo->read_state = pressed ? LV_INDEV_STATE_PRESSED :
				     LV_INDEV_STATE_RELEASED;
	demo->held = pressed ? key : 0;
	lv_indev_read(demo->keypad);
}

void lvgl_keypad_press(struct lvgl_demo *demo, uint32_t key)
{
	if (demo->held)
		lvgl_keypad_feed(demo, demo->held, false);
	lvgl_keypad_feed(demo, key, true);
}

/* LV_KEY_ENTER pressed and released: the focused widget's action. */
static void keypad_enter(struct lvgl_demo *demo)
{
	lvgl_keypad_press(demo, LV_KEY_ENTER);
	lvgl_keypad_feed(demo, LV_KEY_ENTER, false);
}

/* The LVGL key of a phone or keyboard key, or 0 for none. */
static uint32_t lvgl_key(const struct fplinux_ui_key *key)
{
	switch (key->code) {
	case KEY_UP:
		return LV_KEY_UP;
	case KEY_DOWN:
		return LV_KEY_DOWN;
	case KEY_LEFT:
		return LV_KEY_LEFT;
	case KEY_RIGHT:
		return LV_KEY_RIGHT;
	case KEY_OK:
	case KEY_ENTER:
	case KEY_KPENTER:
		return LV_KEY_ENTER;
	case KEY_BACKSPACE:
		return LV_KEY_BACKSPACE;
	case KEY_DELETE:
		return LV_KEY_DEL;
	case KEY_HOME:
		return LV_KEY_HOME;
	case KEY_END:
		return LV_KEY_END;
	case KEY_TAB:
		return key->shift ? LV_KEY_PREV : LV_KEY_NEXT;
	case KEY_NUMERIC_STAR:
		return '*';
	case KEY_NUMERIC_POUND:
		return '#';
	default:
		break;
	}
	if (key->code >= KEY_NUMERIC_0 && key->code <= KEY_NUMERIC_9)
		return '0' + (key->code - KEY_NUMERIC_0);
	/* An external keyboard's character in its active layout. */
	if (key->unicode >= ' ')
		return key->unicode;
	return 0;
}

/* The canvas key handler: every key that no command took. */
static void on_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		   const struct fplinux_ui_key *key, void *context)
{
	struct lvgl_demo *demo = context;
	uint32_t code = lvgl_key(key);
	struct focusable *f;

	(void)ui;
	(void)canvas;
	if (!key->pressed) {
		if (code && code == demo->held)
			lvgl_keypad_feed(demo, code, false);
		return;
	}
	if (!code)
		return;
	/* A widget scrolled out of view, or a real key note, takes no widget keys.
	 * Pages without group members, such as Tabs, still handle their keys. */
	if (demo->spec->key && !demo->unfocused &&
	    (!demo->key_note ||
	     lv_group_get_focused(demo->group) != demo->key_note) &&
	    demo->spec->key(demo, key, code)) {
		settle(demo);
		return;
	}
	f = focused_entry(demo);
	switch (code) {
	case LV_KEY_UP:
	case LV_KEY_DOWN:
		if (f && (f->uses & USES_UP_DOWN))
			lvgl_keypad_press(demo, code);
		else
			page_move(demo, code == LV_KEY_DOWN);
		break;
	case LV_KEY_LEFT:
	case LV_KEY_RIGHT:
		/* Otherwise Left and Right scroll a page, as for any text. */
		if (f && (f->uses & USES_LEFT_RIGHT))
			lvgl_keypad_press(demo, code);
		else if (fplinux_ui_scroll_key(demo->page, key))
			scrolled(demo);
		break;
	case LV_KEY_NEXT:
	case LV_KEY_PREV:
		lvgl_keypad_press(demo, code);
		break;
	case LV_KEY_ENTER:
		/* OK without an OK command does nothing: the legend is empty. */
		break;
	default:
		if (f && (f->uses & USES_TEXT))
			lvgl_keypad_press(demo, code);
		break;
	}
	settle(demo);
}

static void on_command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       int command_id, void *context)
{
	struct lvgl_demo *demo = context;
	lv_obj_t *focused = lv_group_get_focused(demo->group);

	(void)screen;
	switch (command_id) {
	case COMMAND_OK:
		if (!demo->spec->ok || !demo->spec->ok(demo, focused))
			keypad_enter(demo);
		break;
	case COMMAND_NEXT:
		lv_group_focus_next(demo->group);
		/* From a widget scrolled out of view: show the focus again. */
		if (demo->unfocused)
			focus_object(demo, lv_group_get_focused(demo->group));
		break;
	case APIDEMO_COMMAND_BACK:
		if (demo->spec->back && demo->spec->back(demo))
			break;
		/* Deletes the screen and, with its canvas object, demo. */
		fplinux_ui_go_back(ui);
		return;
	default:
		break;
	}
	settle(demo);
}

/* The canvas object goes with its screen: free what the demo made. */
static void area_deleted(lv_event_t *event)
{
	struct lvgl_demo *demo = lv_event_get_user_data(event);

	keypad_delete(demo);
	/* Native popups are screen-layer siblings, not canvas children. */
	if (demo->popup)
		lv_obj_delete(demo->popup);
	/* Delete the widgets now, while their styles and images exist. */
	lv_obj_clean(demo->area);
	for (size_t i = 0; i < demo->buffer_count; i++)
		lv_draw_buf_destroy(demo->buffers[i]);
	styles_reset(&demo->styles);
	if (current == demo)
		current = NULL;
	free(demo);
}

/* The scrolling page of the canvas: a column with the system's indicator. */
static lv_obj_t *page_new(struct lvgl_demo *demo)
{
	const struct fplinux_ui_metrics *m = metrics(demo);
	lv_obj_t *page = lv_obj_create(demo->area);

	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, lv_pct(100), lv_pct(100));
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_left(page, m->margin, 0);
	lv_obj_set_style_pad_right(page, m->gutter, 0);
	lv_obj_set_style_pad_ver(page, m->spacing, 0);
	lv_obj_set_style_pad_row(page, m->spacing, 0);
	apidemo_style_scroll(demo->app, page);
	return page;
}

static void open_widget(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct lvgl_demo *demo = calloc(1, sizeof(*demo));
	struct fplinux_ui_screen *canvas = NULL;

	if (!demo) {
		apidemo_error(app, "calloc");
		return;
	}
	demo->app = app;
	demo->spec = &lvgl_widget_demos[entry->variant];
	/* The device first: a screen not yet pushed cannot be deleted. */
	if (keypad_new(demo))
		canvas = fplinux_ui_canvas_new(app->ui, _(entry->title));
	if (!canvas) {
		keypad_delete(demo);
		free(demo);
		apidemo_error(app, "fplinux_ui_canvas_new");
		return;
	}
	demo->screen = canvas;
	demo->area = fplinux_ui_canvas_object(canvas);
	styles_init(&demo->styles, app->theme, app->metrics);
	lv_obj_add_event_cb(demo->area, area_deleted, LV_EVENT_DELETE, demo);
	demo->page = page_new(demo);
	demo->spec->build(demo);
	/*
	 * A widget that consumes every arrow (a keyboard or canvas, for
	 * example) still has a way to read its key note: Next reaches the
	 * note as a read-only group member, then returns to the controls.
	 */
	if (!demo->spec->rows && demo->focusable_count && demo->key_note)
		lvgl_add_focusable(demo, demo->key_note, OK_NONE, 0);

	apidemo_add_back(canvas);
	if (demo->focusable_count > 1 && !demo->spec->rows) {
		const struct fplinux_ui_command next = {
			.id = COMMAND_NEXT,
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = _("Next"),
		};

		fplinux_ui_screen_add_command(canvas, &next);
	}
	fplinux_ui_screen_set_listener(canvas, on_command, NULL, demo);
	fplinux_ui_canvas_set_key_handler(canvas, on_key, demo);
	fplinux_ui_canvas_set_key_repeat(canvas,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	current = demo;
	apidemo_push(app, canvas, "fplinux_ui_canvas_new");
	/*
	 * A hidden object takes no focus, and the screen was hidden until the
	 * push: focus the first widget now.
	 */
	if (demo->focusable_count)
		lv_group_focus_obj(demo->focusables[0].object);
	lvgl_update_ok(demo);
}

/* The catalogue */

static const struct apidemo_entry entries[WIDGET_COUNT] = {
	{ N_("Animated image"),
	  "lv_animimg · lv_animimg_set_src · lv_animimg_start", open_widget,
	  WIDGET_ANIMIMG },
	{ N_("Arc"), "lv_arc · lv_arc_set_mode · LV_EVENT_VALUE_CHANGED",
	  open_widget, WIDGET_ARC },
	{ N_("Arc label"), "lv_arclabel · lv_arclabel_set_angle_start",
	  open_widget, WIDGET_ARCLABEL },
	{ N_("Bar"), "lv_bar · lv_bar_set_mode · lv_bar_set_value", open_widget,
	  WIDGET_BAR },
	{ N_("Button"), "lv_button · lv_obj_set_checkable · LV_EVENT_CLICKED",
	  open_widget, WIDGET_BUTTON },
	{ N_("Button matrix"),
	  "lv_buttonmatrix · lv_buttonmatrix_set_one_checked", open_widget,
	  WIDGET_BUTTONMATRIX },
	{ N_("Calendar"), "lv_calendar · lv_calendar_add_header_arrow",
	  open_widget, WIDGET_CALENDAR },
	{ N_("Calendar with lists"),
	  "lv_calendar · lv_calendar_add_header_dropdown", open_widget,
	  WIDGET_CALENDAR_LISTS },
	{ N_("Canvas"), "lv_canvas · lv_canvas_init_layer · lv_canvas_set_px",
	  open_widget, WIDGET_CANVAS },
	{ N_("Chart"),
	  "lv_chart · LV_CHART_TYPE_LINE, _BAR · lv_chart_add_cursor",
	  open_widget, WIDGET_CHART },
	{ N_("Checkbox"),
	  "lv_checkbox · lv_obj_set_radio_button · "
	  "LV_EVENT_VALUE_CHANGED",
	  open_widget, WIDGET_CHECKBOX },
	{ N_("Dropdown list"),
	  "lv_dropdown · lv_dropdown_set_symbol · "
	  "lv_dropdown_get_list",
	  open_widget, WIDGET_DROPDOWN },
	{ N_("Image"),
	  "lv_image · fplinux_ui_image_load · "
	  "lv_image_set_rotation",
	  open_widget, WIDGET_IMAGE },
	{ N_("Image button"),
	  "lv_imagebutton · lv_imagebutton_set_src · "
	  "lv_draw_buf_create",
	  open_widget, WIDGET_IMAGEBUTTON },
	{ N_("Keyboard"), "lv_keyboard · lv_keyboard_set_map · lv_textarea",
	  open_widget, WIDGET_KEYBOARD },
	{ N_("Label"),
	  "lv_label · LV_LABEL_LONG_MODE_WRAP, _DOTS, _SCROLL_CIRCULAR",
	  open_widget, WIDGET_LABEL },
	{ N_("LED"), "lv_led · lv_led_toggle · lv_led_set_brightness",
	  open_widget, WIDGET_LED },
	{ N_("Line"), "lv_line · lv_line_set_points · line_dash_width",
	  open_widget, WIDGET_LINE },
	{ N_("List"), "lv_list is deprecated: a flex column of lv_button rows",
	  open_widget, WIDGET_LIST },
	{ N_("Menu"),
	  "lv_menu · lv_menu_page_create · "
	  "lv_menu_set_load_page_event",
	  open_widget, WIDGET_MENU },
	{ N_("Message box"),
	  "lv_msgbox · lv_msgbox_add_footer_button · "
	  "lv_msgbox_close_async",
	  open_widget, WIDGET_MSGBOX },
	{ N_("Roller"),
	  "lv_roller · LV_ROLLER_MODE_INFINITE · "
	  "lv_roller_get_selected_str",
	  open_widget, WIDGET_ROLLER },
	{ N_("Scale"),
	  "lv_scale · lv_scale_add_section · "
	  "lv_scale_set_line_needle_value",
	  open_widget, WIDGET_SCALE },
	{ N_("Slider"),
	  "lv_slider · lv_slider_set_range · LV_EVENT_VALUE_CHANGED",
	  open_widget, WIDGET_SLIDER },
	{ N_("Styled text"),
	  "lv_spangroup · lv_spangroup_add_span · lv_span_get_style",
	  open_widget, WIDGET_SPAN },
	{ N_("Spinbox"),
	  "lv_spinbox · lv_spinbox_set_digit_count · "
	  "lv_spinbox_set_dec_point_pos",
	  open_widget, WIDGET_SPINBOX },
	{ N_("Spinner"), "lv_spinner · lv_spinner_set_anim_params", open_widget,
	  WIDGET_SPINNER },
	{ N_("Switch"),
	  "lv_switch · lv_obj_set_checked · "
	  "LV_EVENT_VALUE_CHANGED",
	  open_widget, WIDGET_SWITCH },
	{ N_("Table"),
	  "lv_table · lv_table_set_cell_value · "
	  "lv_table_get_selected_cell",
	  open_widget, WIDGET_TABLE },
	{ N_("Tabs"), "lv_tabview · lv_tabview_add_tab · lv_tabview_set_active",
	  open_widget, WIDGET_TABVIEW },
	{ N_("Text area"),
	  "lv_textarea · lv_textarea_set_one_line · "
	  "lv_textarea_set_password_mode",
	  open_widget, WIDGET_TEXTAREA },
	{ N_("Tiles"),
	  "lv_tileview · lv_tileview_add_tile · "
	  "lv_tileview_set_tile_by_index",
	  open_widget, WIDGET_TILEVIEW },
	{ N_("Window"), "lv_win · lv_win_add_title · lv_win_add_button",
	  open_widget, WIDGET_WIN },
};

/* Before fplinux_ui_close(): LVGL input devices go before the display. */
static void lvgl_cleanup(struct apidemo *app)
{
	(void)app;
	if (current)
		keypad_delete(current);
}

const struct apidemo_category apidemo_lvgl = {
	.title = N_("LVGL widgets"),
	.icon = APIDEMO_IMAGE_DIR "lvgl-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.cleanup = lvgl_cleanup,
};
