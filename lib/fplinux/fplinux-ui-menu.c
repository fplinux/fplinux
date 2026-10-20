/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fplinux-ui-menu.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-listview.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * The options menu: a brand edge line, an optional title
 * band like fplinux_ui_header() and a plain list with optional glyphs, at
 * the bottom of the application surface just above the soft-key legends.
 */
struct fplinux_ui_menu_view {
	struct fplinux_ui *ui;
	lv_obj_t *popup;
	/* The title band's label, or NULL, and the box of the rows. */
	lv_obj_t *title;
	lv_obj_t *body;
	/* Where the rows start: below the edge and the band. */
	int32_t top;
	size_t count;
	struct fplinux_ui_listview *list;
	int *actions;
	bool chosen;
	bool suspended;
};

/* Lines for Up and Down, pages for Left and Right; never animated. */
bool fplinux_ui_scroll_key(lv_obj_t *object, const struct fplinux_ui_key *key)
{
	int32_t line;
	int32_t step;

	if (!object || key->control || key->alt ||
	    (key->code != KEY_UP && key->code != KEY_DOWN &&
	     key->code != KEY_LEFT && key->code != KEY_RIGHT))
		return false;
	lv_obj_update_layout(object);
	if (lv_obj_get_scroll_top(object) <= 0 &&
	    lv_obj_get_scroll_bottom(object) <= 0)
		return false;
	if (!key->pressed)
		return true;
	line = lv_font_get_line_height(
		       lv_obj_get_style_text_font(object, LV_PART_MAIN)) +
	       lv_obj_get_style_text_line_space(object, LV_PART_MAIN);
	step = line;
	if (key->code == KEY_LEFT || key->code == KEY_RIGHT) {
		step = lv_obj_get_content_height(object) - line;
		if (step < line)
			step = line;
	}
	if (key->code == KEY_DOWN || key->code == KEY_RIGHT)
		step = -step;
	lv_obj_scroll_by_bounded(object, 0, step, LV_ANIM_OFF);
	return true;
}

static void popup_delete_cb(lv_event_t *event)
{
	struct fplinux_ui_menu_view *view = lv_event_get_user_data(event);

	/* The list is the pop-up's child and goes with it. */
	view->popup = NULL;
	view->list = NULL;
}

static void item_chosen(struct fplinux_ui_listview *list, size_t index,
			void *context)
{
	struct fplinux_ui_menu_view *view = context;

	(void)list;
	(void)index;
	view->chosen = true;
}

/* An item without a usable image keeps an empty icon box. */
static void add_icon(struct fplinux_ui_listview *list, size_t index,
		     const void *src, int size)
{
	lv_draw_buf_t *icon;

	if (!src)
		return;
	icon = fplinux_ui_image_decode(src, size, size, false);
	if (icon && !fplinux_ui_listview_set_icon(list, index, icon))
		lv_draw_buf_destroy(icon);
}

static lv_obj_t *add_box(lv_obj_t *parent, int32_t y, int32_t height)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_remove_style_all(box);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_pos(box, 0, y);
	lv_obj_set_size(box, lv_pct(100), height);
	return box;
}

static lv_obj_t *add_title(struct fplinux_ui *ui, lv_obj_t *popup,
			   const char *title)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);
	lv_obj_t *band = add_box(popup, spec->menu_edge, spec->header_height);
	lv_obj_t *label = lv_label_create(band);

	lv_obj_set_style_border_color(band, theme->border, 0);
	lv_obj_set_style_border_width(band, spec->divider_width, 0);
	lv_obj_set_style_border_side(band, LV_BORDER_SIDE_BOTTOM, 0);
	fplinux_ui_title_init(label, theme->font_large);
	lv_obj_set_width(label, fplinux_ui_width(ui) - 2 * spec->margin);
	lv_label_set_text(label, title);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, spec->margin, 0);
	fplinux_ui_title_scroll(label);
	return label;
}

/*
 * Edge, band and rows, at most the whole surface above the legend bar of a
 * fullscreen application.
 */
static int32_t popup_height(struct fplinux_ui *ui, int32_t top, size_t count)
{
	int32_t surface = fplinux_ui_modal_height(ui);
	int32_t row = fplinux_ui_spec(ui)->row_height;
	int32_t height = surface;

	if (count < (size_t)(surface - top) / (size_t)row + 1)
		height = top + (int32_t)count * row;
	if (height > surface)
		height = surface;
	return height;
}

/* Sized to its items up to the whole surface; the list scrolls beyond. */
struct fplinux_ui_menu_view *
fplinux_ui_menu_view_create(struct fplinux_ui *ui, const char *title,
			    const struct fplinux_ui_menu_item *items,
			    size_t count)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);
	lv_display_t *display = lv_obj_get_display(fplinux_ui_screen(ui));
	int32_t surface = fplinux_ui_modal_height(ui);
	int32_t top = spec->menu_edge;
	struct fplinux_ui_menu_view *view;
	lv_obj_t *popup;
	lv_obj_t *edge;
	lv_obj_t *body;
	int32_t height;

	if (!items || !count) {
		errno = EINVAL;
		return NULL;
	}
	for (size_t i = 0; i < count; ++i)
		if (!items[i].text) {
			errno = EINVAL;
			return NULL;
		}
	view = calloc(1, sizeof(*view));
	if (!view)
		return NULL;
	view->ui = ui;
	view->actions = calloc(count, sizeof(*view->actions));
	if (!view->actions) {
		free(view);
		return NULL;
	}
	if (title && title[0])
		top += spec->header_height;
	height = popup_height(ui, top, count);
	view->top = top;
	view->count = count;

	popup = lv_obj_create(lv_display_get_layer_top(display));
	view->popup = popup;
	lv_obj_add_event_cb(popup, popup_delete_cb, LV_EVENT_DELETE, view);
	lv_obj_remove_style_all(popup);
	lv_obj_set_scrollable(popup, false);
	lv_obj_set_size(popup, fplinux_ui_width(ui), height);
	lv_obj_set_pos(popup, 0, surface - height);
	lv_obj_set_style_bg_color(popup, theme->popup, 0);
	lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(popup, theme->foreground, 0);
	lv_obj_set_style_text_font(popup, theme->font, 0);
	if (!fplinux_ui_scrim_attach(ui, popup)) {
		fplinux_ui_menu_view_delete(view);
		return NULL;
	}
	edge = add_box(popup, 0, spec->menu_edge);
	lv_obj_set_style_bg_color(edge, theme->brand, 0);
	lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
	if (title && title[0])
		view->title = add_title(ui, popup, title);
	body = add_box(popup, top, height - top);
	view->body = body;
	view->list =
		fplinux_ui_listview_create(ui, body, FPLINUX_UI_LISTVIEW_PLAIN);
	if (!view->list) {
		int error = errno;

		fplinux_ui_menu_view_delete(view);
		errno = error;
		return NULL;
	}
	lv_obj_set_style_pad_ver(fplinux_ui_listview_object(view->list), 0, 0);
	/*
	 * Inactive while filled: the first row measures its text for the
	 * marquee once the rows have their icon boxes, not on the wider row
	 * before them.
	 */
	fplinux_ui_listview_set_active(view->list, false);
	for (size_t i = 0; i < count; ++i) {
		view->actions[i] = items[i].action;
		fplinux_ui_listview_add(view->list, items[i].text, NULL);
		add_icon(view->list, i, items[i].icon, spec->row_icon_size);
	}
	fplinux_ui_listview_set_active(view->list, true);
	fplinux_ui_listview_set_callbacks(view->list, NULL, item_chosen, view);
	fplinux_ui_text_menu_suspend(ui);
	fplinux_ui_screens_suspend(*fplinux_ui_screens_slot(ui));
	view->suspended = true;
	return view;
}

/* OK chooses the selected item; the right soft key or Escape cancels. */
bool fplinux_ui_menu_view_key(struct fplinux_ui_menu_view *view,
			      const struct fplinux_ui_key *key, int *action)
{
	/* Someone deleted the pop-up: the menu is over. */
	if (!view->list) {
		*action = FPLINUX_UI_MENU_CANCELLED;
		return true;
	}
	if (key->pressed &&
	    (fplinux_ui_is_softkey(key, false) || key->code == KEY_ESC)) {
		*action = FPLINUX_UI_MENU_CANCELLED;
		return true;
	}
	view->chosen = false;
	fplinux_ui_listview_key(view->list, key);
	if (!view->chosen)
		return false;
	*action = view->actions[fplinux_ui_listview_selected(view->list)];
	return true;
}

/* At the bottom of the new surface; the selected item stays in view. */
void fplinux_ui_menu_view_resize(struct fplinux_ui_menu_view *view)
{
	const struct fplinux_ui_profile_spec *spec;
	int32_t surface;
	int32_t height;
	lv_obj_t *row;

	if (!view || !view->popup)
		return;
	spec = fplinux_ui_spec(view->ui);
	surface = fplinux_ui_modal_height(view->ui);
	height = popup_height(view->ui, view->top, view->count);
	lv_obj_set_size(view->popup, fplinux_ui_width(view->ui), height);
	lv_obj_set_pos(view->popup, 0, surface - height);
	if (view->title)
		lv_obj_set_width(view->title,
				 fplinux_ui_width(view->ui) - 2 * spec->margin);
	lv_obj_set_height(view->body, height - view->top);
	lv_obj_update_layout(view->popup);
	row = fplinux_ui_listview_selected_row(view->list);
	if (row)
		lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

void fplinux_ui_menu_view_delete(struct fplinux_ui_menu_view *view)
{
	if (!view)
		return;
	if (view->popup)
		lv_obj_delete(view->popup);
	if (view->suspended)
		fplinux_ui_text_menu_reclaim(view->ui);
	free(view->actions);
	free(view);
}
