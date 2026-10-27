/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fplinux-ui-listview.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-screen-model.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>

/* An inline detail takes at most half of the row, as a popup choice's value. */
#define DETAIL_MAX_PERCENT 50

/* The selected row and its labels and icon carry this state for colours. */
#define ROW_SELECTED LV_STATE_USER_1
/* A checked row of a check list, or the chosen row of a radio list. */
#define ROW_MARKED LV_STATE_CHECKED
/* The icon box carries its image role through stored row edits and reuse. */
#define ICON_APP_TILE LV_STATE_USER_2
/* Stored rows can append a status image after the text/detail body. */
#define ROW_TRAILING LV_STATE_USER_3

/*
 * Each row is a child of the list object in index order: the marker box
 * (check and radio lists only), the icon box (once any row has an icon),
 * then the text label and an optional inline detail label, or, once the
 * list shows details on a second line, a column with the text label over
 * an optional detail label. A virtual list keeps a pool of such rows after
 * a spacer that gives the content its full height.
 */
struct fplinux_ui_listview {
	struct fplinux_ui *ui;
	const struct fplinux_ui_theme *theme;
	const struct fplinux_ui_profile_spec *spec;
	lv_obj_t *object;
	enum fplinux_ui_listview_marker marker;
	size_t count;
	size_t selected;
	/* The marked row of a radio list, or SIZE_MAX. */
	size_t radio;
	bool selectable;
	/* The focused row takes the marquee. */
	bool active;
	/* Every row has an icon box. */
	bool icons;
	/* Details sit at the right of the text, not under it. */
	bool inline_detail;
	/* Every row has a text column and the detail pitch. */
	bool details;
	int32_t pitch;
	fplinux_ui_listview_cb select;
	fplinux_ui_listview_cb activate;
	void *context;
	/* Presses the list used, by source and code; it takes their releases. */
	unsigned char consumed[2][KEY_CNT / 8];
	lv_style_t row_style;
	lv_style_t selected_style;
	lv_style_t marker_style;
	lv_style_t text_style;
	lv_style_t column_style;
	lv_style_t detail_style;
	lv_style_t detail_line_style;
	lv_style_t detail_selected_style;
	lv_style_t icon_style;
	lv_style_t icon_selected_style;
	/* A virtual list: its rows come from row_cb into the slots. */
	bool is_virtual;
	/* row_cb runs only once the list has been active (on top). */
	bool started;
	fplinux_ui_listview_row_cb row_cb;
	void *row_context;
	struct fplinux_ui_virtual_window window;
	lv_obj_t *spacer;
	lv_obj_t *slots[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	size_t slot_count;
};

static void marker_draw(lv_event_t *event)
{
	const struct fplinux_ui_listview *list = lv_event_get_user_data(event);
	lv_obj_t *object = lv_event_get_target_obj(event);
	lv_layer_t *layer;
	lv_draw_rect_dsc_t outline;
	lv_area_t box;
	lv_color_t colour;
	int32_t size, stroke;
	bool radio;

	if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN)
		return;
	layer = lv_event_get_layer(event);
	lv_obj_get_coords(object, &box);
	size = lv_area_get_width(&box);
	stroke = lv_obj_get_style_line_width(object, LV_PART_MAIN);
	colour = lv_obj_get_style_text_color(object, LV_PART_MAIN);
	radio = list->marker == FPLINUX_UI_LISTVIEW_RADIO;
	lv_draw_rect_dsc_init(&outline);
	outline.bg_opa = LV_OPA_TRANSP;
	outline.border_color = colour;
	outline.border_width = stroke;
	outline.radius = radio ? LV_RADIUS_CIRCLE : 0;
	lv_draw_rect(layer, &outline, &box);
	if (!lv_obj_has_state(object, ROW_MARKED))
		return;
	if (radio) {
		/* A half-diameter dot leaves a quarter-diameter gap to the edge. */
		int32_t dot = size / 2;

		box.x1 += (size - dot) / 2;
		box.y1 += (size - dot) / 2;
		box.x2 = box.x1 + dot - 1;
		box.y2 = box.y1 + dot - 1;
		outline.bg_opa = LV_OPA_COVER;
		outline.bg_color = colour;
		outline.border_width = 0;
		lv_draw_rect(layer, &outline, &box);
	} else {
		lv_draw_line_dsc_t check;

		/* The check stays within the middle half of the square. */
		lv_draw_line_dsc_init(&check);
		check.color = colour;
		check.width = stroke;
		check.round_start = true;
		check.round_end = true;
		check.p1 = (lv_point_precise_t){ box.x1 + size / 4,
						 box.y1 + size / 2 };
		check.p2 = (lv_point_precise_t){ box.x1 + 7 * size / 16,
						 box.y1 + 3 * size / 4 };
		lv_draw_line(layer, &check);
		check.p1 = check.p2;
		check.p2 = (lv_point_precise_t){ box.x1 + 3 * size / 4,
						 box.y1 + size / 4 };
		lv_draw_line(layer, &check);
	}
}

/* The row object showing index, or NULL (a virtual row out of the pool). */
static lv_obj_t *row_at(const struct fplinux_ui_listview *list, size_t index)
{
	if (!list->is_virtual)
		return lv_obj_get_child(list->object, (int32_t)index);
	for (size_t slot = 0; slot < list->slot_count; ++slot)
		if (list->window.bound[slot] == index)
			return list->slots[slot];
	return NULL;
}

/* The child index of the icon box, when the rows have one. */
static int32_t icon_slot(const struct fplinux_ui_listview *list)
{
	return list->marker == FPLINUX_UI_LISTVIEW_PLAIN ? 0 : 1;
}

/* The text label, or with details the column holding it. */
static lv_obj_t *row_body(const struct fplinux_ui_listview *list, lv_obj_t *row)
{
	return lv_obj_get_child(row, icon_slot(list) + (list->icons ? 1 : 0));
}

static lv_obj_t *row_text(const struct fplinux_ui_listview *list, lv_obj_t *row)
{
	lv_obj_t *body = row_body(list, row);

	return list->details ? lv_obj_get_child(body, 0) : body;
}

static lv_obj_t *row_trailing(lv_obj_t *row)
{
	return lv_obj_has_state(row, ROW_TRAILING) ? lv_obj_get_child(row, -1) :
						     NULL;
}

/* NULL when the row has no detail. */
static lv_obj_t *row_detail(const struct fplinux_ui_listview *list,
			    lv_obj_t *row)
{
	lv_obj_t *detail;

	if (list->details)
		return lv_obj_get_child(row_body(list, row), 1);
	detail = lv_obj_get_child(row, icon_slot(list) + (list->icons ? 2 : 1));
	return detail == row_trailing(row) ? NULL : detail;
}

/* NULL when the row shows no icon. */
static lv_obj_t *row_icon(const struct fplinux_ui_listview *list, lv_obj_t *row)
{
	if (!list->icons)
		return NULL;
	return lv_obj_get_child(lv_obj_get_child(row, icon_slot(list)), 0);
}

static void init_styles(struct fplinux_ui_listview *list)
{
	const struct fplinux_ui_theme *theme = list->theme;
	const struct fplinux_ui_profile_spec *spec = list->spec;
	int32_t line = lv_font_get_line_height(theme->font);

	lv_style_init(&list->row_style);
	lv_style_set_width(&list->row_style, lv_pct(100));
	lv_style_set_height(&list->row_style, list->pitch);
	lv_style_set_pad_hor(&list->row_style, spec->row_padding);
	lv_style_set_pad_column(&list->row_style, spec->row_gap);
	lv_style_set_bg_color(&list->row_style, theme->accent);
	lv_style_set_bg_opa(&list->row_style, LV_OPA_TRANSP);
	lv_style_set_border_color(&list->row_style, theme->border);
	lv_style_set_border_width(&list->row_style, spec->divider_width);
	lv_style_set_border_side(&list->row_style, LV_BORDER_SIDE_BOTTOM);
	lv_style_set_text_color(&list->row_style, theme->foreground);
	lv_style_set_layout(&list->row_style, LV_LAYOUT_FLEX);
	lv_style_set_flex_flow(&list->row_style, LV_FLEX_FLOW_ROW);
	lv_style_set_flex_cross_place(&list->row_style, LV_FLEX_ALIGN_CENTER);
	lv_style_set_flex_track_place(&list->row_style, LV_FLEX_ALIGN_CENTER);

	/* The selected row is inverted over its full pitch, divider included. */
	lv_style_init(&list->selected_style);
	lv_style_set_bg_opa(&list->selected_style, LV_OPA_COVER);
	lv_style_set_border_color(&list->selected_style, theme->accent);
	lv_style_set_text_color(&list->selected_style, theme->accent_text);

	/* Both kinds reserve the icon-size square in either mark state. */
	lv_style_init(&list->marker_style);
	lv_style_set_width(&list->marker_style, spec->row_icon_size);
	lv_style_set_height(&list->marker_style, spec->row_icon_size);
	lv_style_set_line_width(&list->marker_style, spec->focus_width);

	/* One line high, so that a longer text ends with dots. */
	lv_style_init(&list->text_style);
	lv_style_set_height(&list->text_style, line);
	lv_style_set_flex_grow(&list->text_style, 1);

	/*
	 * Text over detail, each one line, filling the row's width. The column
	 * is a flex container itself: only a layout clears the size LVGL's row
	 * layout left on the reparented text, so without one the text keeps
	 * the row's width from before the icon box and overflows the row.
	 */
	lv_style_init(&list->column_style);
	lv_style_set_height(&list->column_style, 2 * line);
	lv_style_set_flex_grow(&list->column_style, 1);
	lv_style_set_layout(&list->column_style, LV_LAYOUT_FLEX);
	lv_style_set_flex_flow(&list->column_style, LV_FLEX_FLOW_COLUMN);

	lv_style_init(&list->detail_style);
	lv_style_set_height(&list->detail_style, line);
	lv_style_set_max_width(&list->detail_style, lv_pct(DETAIL_MAX_PERCENT));
	lv_style_set_text_align(&list->detail_style, LV_TEXT_ALIGN_RIGHT);
	lv_style_set_text_color(&list->detail_style, theme->muted);

	lv_style_init(&list->detail_line_style);
	lv_style_set_width(&list->detail_line_style, lv_pct(100));
	lv_style_set_height(&list->detail_line_style, line);
	lv_style_set_y(&list->detail_line_style, line);
	lv_style_set_text_color(&list->detail_line_style, theme->muted);

	lv_style_init(&list->detail_selected_style);
	lv_style_set_text_color(&list->detail_selected_style,
				theme->accent_text);

	/* An alpha-only icon takes the row's text colour. */
	lv_style_init(&list->icon_style);
	lv_style_set_image_recolor(&list->icon_style, theme->foreground);
	lv_style_init(&list->icon_selected_style);
	lv_style_set_image_recolor(&list->icon_selected_style,
				   theme->accent_text);
}

static void delete_cb(lv_event_t *event)
{
	struct fplinux_ui_listview *list = lv_event_get_user_data(event);

	/* The rows use the list's styles: delete them before the styles. */
	lv_obj_clean(list->object);
	lv_style_reset(&list->row_style);
	lv_style_reset(&list->selected_style);
	lv_style_reset(&list->marker_style);
	lv_style_reset(&list->text_style);
	lv_style_reset(&list->column_style);
	lv_style_reset(&list->detail_style);
	lv_style_reset(&list->detail_line_style);
	lv_style_reset(&list->detail_selected_style);
	lv_style_reset(&list->icon_style);
	lv_style_reset(&list->icon_selected_style);
	free(list);
}

static void size_changed_cb(lv_event_t *event);
static void mark(struct fplinux_ui_listview *list, size_t index, bool marked);

struct fplinux_ui_listview *
fplinux_ui_listview_create(struct fplinux_ui *ui, lv_obj_t *parent,
			   enum fplinux_ui_listview_marker marker)
{
	const struct fplinux_ui_profile_spec *spec;
	struct fplinux_ui_listview *list;
	lv_obj_t *object;

	if (!ui || !parent ||
	    (marker != FPLINUX_UI_LISTVIEW_PLAIN &&
	     marker != FPLINUX_UI_LISTVIEW_CHECK &&
	     marker != FPLINUX_UI_LISTVIEW_RADIO)) {
		errno = EINVAL;
		return NULL;
	}
	list = calloc(1, sizeof(*list));
	if (!list)
		return NULL;
	spec = fplinux_ui_spec(ui);
	list->ui = ui;
	list->theme = fplinux_ui_theme(ui);
	list->spec = spec;
	list->marker = marker;
	list->radio = SIZE_MAX;
	list->selectable = true;
	list->active = true;
	list->pitch = spec->row_height;
	init_styles(list);
	object = lv_obj_create(parent);
	list->object = object;
	lv_obj_remove_style_all(object);
	lv_obj_set_size(object, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_left(object, 0, 0);
	lv_obj_set_style_text_font(object, list->theme->font, 0);
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(ui, object);
	lv_obj_add_event_cb(object, delete_cb, LV_EVENT_DELETE, list);
	return list;
}

lv_obj_t *fplinux_ui_listview_object(struct fplinux_ui_listview *list)
{
	return list->object;
}

void fplinux_ui_listview_set_inline_detail(struct fplinux_ui_listview *list,
					   bool inline_detail)
{
	list->inline_detail = inline_detail;
}

static void set_row_state(const struct fplinux_ui_listview *list, lv_obj_t *row,
			  bool selected)
{
	lv_obj_t *detail = row_detail(list, row);
	lv_obj_t *icon = row_icon(list, row);

	lv_obj_set_state(row, ROW_SELECTED, selected);
	if (detail)
		lv_obj_set_state(detail, ROW_SELECTED, selected);
	if (icon)
		lv_obj_set_state(icon, ROW_SELECTED, selected);
	if (row_trailing(row))
		lv_obj_set_state(row_trailing(row), ROW_SELECTED, selected);
	if (list->icons)
		lv_obj_set_state(lv_obj_get_child(row, icon_slot(list)),
				 ROW_SELECTED, selected);
}

/* Draws or clears the selection; a drawn selection is scrolled into view. */
static void show_selected(struct fplinux_ui_listview *list, bool shown)
{
	lv_obj_t *row = list->count ? row_at(list, list->selected) : NULL;
	lv_obj_t *text;

	if (!row)
		return;
	text = row_text(list, row);
	set_row_state(list, row, shown);
	if (shown && list->active)
		fplinux_ui_marquee_focus(list->ui, text, row_detail(list, row));
	else
		fplinux_ui_marquee_blur(list->ui, text);
	if (shown && !list->is_virtual)
		lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static void start_virtual(struct fplinux_ui_listview *list);

void fplinux_ui_listview_set_active(struct fplinux_ui_listview *list,
				    bool active)
{
	list->active = active;
	if (active && list->is_virtual && !list->started)
		start_virtual(list);
	show_selected(list, list->selectable);
}

static void move_selection(struct fplinux_ui_listview *list, size_t index)
{
	show_selected(list, false);
	list->selected = index;
	show_selected(list, true);
}

/* Puts rows from first on at their index's place. */
static void place_rows(struct fplinux_ui_listview *list, size_t first)
{
	for (size_t i = first; i < list->count; ++i)
		lv_obj_set_y(row_at(list, i), (int32_t)i * list->pitch);
}

static void set_pitch(struct fplinux_ui_listview *list, int32_t pitch)
{
	list->pitch = pitch;
	lv_style_set_height(&list->row_style, pitch);
	lv_obj_report_style_change(&list->row_style);
}

/* Puts the row's text label into a column for a detail under it. */
static void add_column(struct fplinux_ui_listview *list, lv_obj_t *row,
		       lv_obj_t *text)
{
	lv_obj_t *column = lv_obj_create(row);

	lv_obj_remove_style_all(column);
	lv_obj_add_style(column, &list->column_style, 0);
	lv_obj_set_scrollable(column, false);
	lv_obj_move_to_index(column, lv_obj_get_index(text));
	lv_obj_set_parent(text, column);
	/* One line high in the column, not grown to the column's height. */
	lv_obj_set_style_flex_grow(text, 0, 0);
	lv_obj_set_width(text, lv_pct(100));
	lv_obj_set_height(
		column,
		lv_font_get_line_height(lv_obj_get_style_text_font(text, 0)) +
			lv_font_get_line_height(list->theme->font));
}

/*
 * The first detail on a second line moves every row to the detail pitch;
 * a former inline detail stays the row's detail.
 */
static void use_details(struct fplinux_ui_listview *list)
{
	size_t rows = list->is_virtual ? list->slot_count : list->count;

	for (size_t i = 0; i < rows; ++i) {
		lv_obj_t *row = list->is_virtual ? list->slots[i] :
						   row_at(list, i);

		add_column(list, row, row_text(list, row));
	}
	list->details = true;
	set_pitch(list, list->spec->row_height_detail);
	if (!list->is_virtual)
		place_rows(list, 0);
}

static void set_detail(struct fplinux_ui_listview *list, lv_obj_t *row,
		       const char *detail)
{
	lv_obj_t *label, *trailing = row_trailing(row);

	if (detail && detail[0] && !list->inline_detail && !list->details)
		use_details(list);
	label = row_detail(list, row);
	if (!detail || !detail[0]) {
		if (label) {
			/* A focused row's marquee goes on with its text alone. */
			fplinux_ui_marquee_changed(list->ui,
						   row_text(list, row), NULL);
			fplinux_ui_marquee_blur(list->ui, label);
			lv_obj_delete(label);
		}
		return;
	}
	if (!label) {
		label = lv_label_create(list->details ? row_body(list, row) :
							row);
		if (!list->details && trailing)
			lv_obj_move_to_index(label, lv_obj_get_index(trailing));
		lv_obj_add_style(label,
				 list->details ? &list->detail_line_style :
						 &list->detail_style,
				 0);
		lv_obj_add_style(label, &list->detail_selected_style,
				 ROW_SELECTED);
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_state(label, ROW_SELECTED,
				 lv_obj_has_state(row, ROW_SELECTED));
	}
	lv_label_set_text(label, detail);
}

static void tile_ring_draw(lv_event_t *event)
{
	const struct fplinux_ui_listview *list = lv_event_get_user_data(event);
	lv_obj_t *box = lv_event_get_target_obj(event);
	lv_draw_rect_dsc_t ring;
	lv_area_t area;
	int32_t reach = list->spec->list_tile_ring_gap +
			list->spec->list_tile_ring_width;

	if (!lv_obj_has_state(box, ICON_APP_TILE) ||
	    !lv_obj_has_state(box, ROW_SELECTED))
		return;
	lv_obj_get_coords(box, &area);
	lv_area_increase(&area, reach, reach);
	lv_draw_rect_dsc_init(&ring);
	ring.bg_color = list->theme->tile_ring;
	ring.radius = list->spec->row_tile_radius + reach;
	lv_draw_rect(lv_event_get_layer(event), &ring, &area);
}

static void add_icon_box(struct fplinux_ui_listview *list, lv_obj_t *row)
{
	lv_obj_t *box = lv_obj_create(row);

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, list->spec->row_icon_size,
			list->spec->row_icon_size);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_overflow_visible(box, true);
	lv_obj_set_ext_draw_size(box, list->spec->list_tile_ring_gap +
					      list->spec->list_tile_ring_width);
	lv_obj_add_event_cb(box, tile_ring_draw, LV_EVENT_DRAW_MAIN, list);
	lv_obj_set_state(box, ROW_SELECTED,
			 lv_obj_has_state(row, ROW_SELECTED));
	lv_obj_move_to_index(box, icon_slot(list));
}

/*
 * The first icon gives every row a box. The focused row's text is narrower
 * then: its marquee measures it again, as after a new text.
 */
static void use_icons(struct fplinux_ui_listview *list)
{
	size_t rows = list->is_virtual ? list->slot_count : list->count;
	lv_obj_t *focused;

	list->icons = true;
	for (size_t i = 0; i < rows; ++i)
		add_icon_box(list, list->is_virtual ? list->slots[i] :
						      row_at(list, i));
	focused = list->count ? row_at(list, list->selected) : NULL;
	if (focused)
		fplinux_ui_marquee_changed(list->ui, row_text(list, focused),
					   row_detail(list, focused));
}

static void icon_delete_cb(lv_event_t *event)
{
	lv_draw_buf_destroy(lv_event_get_user_data(event));
}

/* Replaces the row's icon; icon belongs to the row afterwards. */
static void show_icon(struct fplinux_ui_listview *list, lv_obj_t *row,
		      lv_draw_buf_t *icon, enum fplinux_ui_list_icon_kind kind)
{
	lv_obj_t *box = lv_obj_get_child(row, icon_slot(list));
	lv_obj_t *image;

	lv_obj_clean(box);
	lv_obj_set_state(box, ICON_APP_TILE,
			 icon && icon->header.cf != LV_COLOR_FORMAT_A8 &&
				 kind == FPLINUX_UI_LIST_ICON_APP_TILE);
	if (!icon)
		return;
	image = lv_image_create(box);
	lv_obj_add_style(image, &list->icon_style, 0);
	lv_obj_add_style(image, &list->icon_selected_style, ROW_SELECTED);
	lv_obj_set_state(image, ROW_SELECTED,
			 lv_obj_has_state(row, ROW_SELECTED));
	lv_image_set_src(image, icon);
	lv_obj_center(image);
	lv_obj_add_event_cb(image, icon_delete_cb, LV_EVENT_DELETE, icon);
}

bool fplinux_ui_listview_set_icon(struct fplinux_ui_listview *list,
				  size_t index, lv_draw_buf_t *icon,
				  enum fplinux_ui_list_icon_kind kind)
{
	if (list->is_virtual || index >= list->count ||
	    (kind != FPLINUX_UI_LIST_ICON_IMAGE &&
	     kind != FPLINUX_UI_LIST_ICON_APP_TILE)) {
		errno = EINVAL;
		return false;
	}
	if (!list->icons && icon)
		use_icons(list);
	if (!list->icons)
		return true;
	show_icon(list, row_at(list, index), icon, kind);
	return true;
}

bool fplinux_ui_listview_set_emphasis(struct fplinux_ui_listview *list,
				      size_t index, bool emphasis)
{
	const lv_font_t *font = emphasis ? list->theme->font_large :
					   list->theme->font;
	lv_obj_t *row, *text;
	int32_t line;

	if (list->is_virtual || index >= list->count) {
		errno = EINVAL;
		return false;
	}
	row = row_at(list, index);
	text = row_text(list, row);
	if (lv_obj_get_style_text_font(text, 0) == font)
		return true;
	line = lv_font_get_line_height(font);
	lv_obj_set_style_text_font(text, font, 0);
	lv_obj_set_height(text, line);
	if (list->details)
		lv_obj_set_height(
			row_body(list, row),
			line + lv_font_get_line_height(list->theme->font));
	fplinux_ui_marquee_changed(list->ui, text, row_detail(list, row));
	return true;
}

bool fplinux_ui_listview_set_trailing_icon(struct fplinux_ui_listview *list,
					   size_t index, const void *source)
{
	lv_obj_t *row, *image;

	if (list->is_virtual || index >= list->count) {
		errno = EINVAL;
		return false;
	}
	row = row_at(list, index);
	image = row_trailing(row);
	if (!source) {
		if (image) {
			lv_obj_remove_state(row, ROW_TRAILING);
			lv_obj_delete(image);
		}
	} else {
		if (!image) {
			image = lv_image_create(row);
			if (!image) {
				errno = ENOMEM;
				return false;
			}
			lv_obj_add_state(row, ROW_TRAILING);
			lv_obj_add_style(image, &list->icon_style, 0);
			lv_obj_add_style(image, &list->icon_selected_style,
					 ROW_SELECTED);
			lv_obj_set_size(image, list->spec->row_icon_size,
					list->spec->row_icon_size);
			lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_set_state(image, ROW_SELECTED,
					 lv_obj_has_state(row, ROW_SELECTED));
		}
		lv_image_set_src(image, source);
	}
	fplinux_ui_marquee_changed(list->ui, row_text(list, row),
				   row_detail(list, row));
	return true;
}

static void separator_draw(lv_event_t *event)
{
	const struct fplinux_ui_listview *list = lv_event_get_user_data(event);
	lv_obj_t *row = lv_event_get_target_obj(event);
	lv_draw_rect_dsc_t separator;
	lv_area_t area;

	if (!lv_obj_get_index(row))
		return;
	lv_obj_get_coords(row, &area);
	area.y2 = area.y1 + list->spec->divider_width - 1;
	lv_draw_rect_dsc_init(&separator);
	separator.bg_color = list->theme->border;
	separator.bg_opa = LV_OPA_COVER;
	lv_draw_rect(lv_event_get_layer(event), &separator, &area);
}

bool fplinux_ui_listview_set_separator_before(struct fplinux_ui_listview *list,
					      size_t index, bool separator)
{
	lv_obj_t *row;

	if (list->is_virtual || index >= list->count) {
		errno = EINVAL;
		return false;
	}
	row = row_at(list, index);
	lv_obj_remove_event_cb(row, separator_draw);
	if (separator)
		lv_obj_add_event_cb(row, separator_draw, LV_EVENT_DRAW_MAIN_END,
				    list);
	lv_obj_invalidate(row);
	return true;
}

/* A new row with its text at the end of parent's children. */
static lv_obj_t *make_row(struct fplinux_ui_listview *list, const char *text)
{
	lv_obj_t *row = lv_obj_create(list->object);
	lv_obj_t *label;

	lv_obj_remove_style_all(row);
	lv_obj_add_style(row, &list->row_style, 0);
	lv_obj_add_style(row, &list->selected_style, ROW_SELECTED);
	lv_obj_set_scrollable(row, false);
	if (list->marker != FPLINUX_UI_LISTVIEW_PLAIN) {
		lv_obj_t *marker = lv_obj_create(row);

		lv_obj_remove_style_all(marker);
		lv_obj_add_style(marker, &list->marker_style, 0);
		lv_obj_set_scrollable(marker, false);
		lv_obj_set_clickable(marker, false);
		lv_obj_add_event_cb(marker, marker_draw, LV_EVENT_DRAW_MAIN,
				    list);
	}
	if (list->icons)
		add_icon_box(list, row);
	label = lv_label_create(row);
	lv_obj_add_style(label, &list->text_style, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(label, text);
	if (list->details)
		add_column(list, row, label);
	return row;
}

/* Scrolls the content by dy, within its bounds, then shows the focus. */
static void keep_focus(struct fplinux_ui_listview *list, int32_t dy)
{
	lv_obj_t *row;

	if (dy)
		lv_obj_scroll_by_bounded(list->object, 0, dy, LV_ANIM_OFF);
	if (!list->count || !list->selectable)
		return;
	row = row_at(list, list->selected);
	lv_obj_update_layout(list->object);
	lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

/* Returns the new row's index, or SIZE_MAX with errno for a NULL text. */
size_t fplinux_ui_listview_add(struct fplinux_ui_listview *list,
			       const char *text, const char *detail)
{
	size_t index = list->count;
	bool had_details = list->details;
	lv_obj_t *row;

	if (!text || list->is_virtual) {
		errno = EINVAL;
		return SIZE_MAX;
	}
	row = make_row(list, text);
	lv_obj_set_y(row, (int32_t)index * list->pitch);
	if (index)
		lv_obj_set_style_border_opa(row_at(list, index - 1),
					    LV_OPA_COVER, 0);
	lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
	++list->count;
	set_detail(list, row, detail);
	if (!index && list->marker == FPLINUX_UI_LISTVIEW_RADIO)
		fplinux_ui_listview_set_marked(list, 0, true);
	if (!index && list->selectable)
		show_selected(list, true);
	else if (!had_details && list->details)
		keep_focus(list, 0);
	return index;
}

size_t fplinux_ui_listview_insert(struct fplinux_ui_listview *list,
				  size_t index, const char *text,
				  const char *detail)
{
	size_t focus = list->selected;
	lv_obj_t *row;

	if (list->is_virtual || !text || index > list->count) {
		errno = EINVAL;
		return SIZE_MAX;
	}
	if (index == list->count)
		return fplinux_ui_listview_add(list, text, detail);
	row = make_row(list, text);
	lv_obj_move_to_index(row, (int32_t)index);
	++list->count;
	set_detail(list, row, detail);
	place_rows(list, index);
	list->selected = fplinux_ui_index_inserted(focus, index, 1);
	if (list->radio != SIZE_MAX)
		list->radio = fplinux_ui_index_inserted(list->radio, index, 1);
	/* Rows above the focus moved it down: the content follows. */
	keep_focus(list, index <= focus ? -list->pitch : 0);
	return index;
}

bool fplinux_ui_listview_remove(struct fplinux_ui_listview *list, size_t index)
{
	size_t focus = list->selected;
	bool focused = index == focus;
	size_t radio;

	if (list->is_virtual || index >= list->count) {
		errno = EINVAL;
		return false;
	}
	if (focused)
		show_selected(list, false);
	lv_obj_delete(row_at(list, index));
	--list->count;
	if (list->count)
		lv_obj_set_style_border_opa(row_at(list, list->count - 1),
					    LV_OPA_TRANSP, 0);
	place_rows(list, index);
	list->selected =
		fplinux_ui_focus_deleted(focus, index, 1, list->count + 1);
	radio = list->radio;
	list->radio = fplinux_ui_index_deleted(radio, index, 1);
	/* A radio list with rows always has its one mark. */
	if (radio == index && list->selected != SIZE_MAX) {
		list->radio = list->selected;
		mark(list, list->radio, true);
	}
	if (list->selected == SIZE_MAX) {
		list->selected = 0;
		return true;
	}
	if (focused && list->selectable)
		show_selected(list, true);
	keep_focus(list, index < focus ? list->pitch : 0);
	return true;
}

bool fplinux_ui_listview_set_text(struct fplinux_ui_listview *list,
				  size_t index, const char *text,
				  const char *detail)
{
	bool had_details = list->details;
	lv_obj_t *row;

	if (list->is_virtual || index >= list->count || !text) {
		errno = EINVAL;
		return false;
	}
	row = row_at(list, index);
	lv_label_set_text(row_text(list, row), text);
	set_detail(list, row, detail);
	/* A focused row's marquee starts over with the new text. */
	fplinux_ui_marquee_changed(list->ui, row_text(list, row),
				   row_detail(list, row));
	if (!had_details && list->details)
		keep_focus(list, 0);
	return true;
}

void fplinux_ui_listview_clear(struct fplinux_ui_listview *list)
{
	if (list->is_virtual)
		return;
	lv_obj_clean(list->object);
	list->count = 0;
	list->selected = 0;
	list->radio = SIZE_MAX;
	list->icons = false;
	if (list->details) {
		list->details = false;
		set_pitch(list, list->spec->row_height);
	}
}

size_t fplinux_ui_listview_count(const struct fplinux_ui_listview *list)
{
	return list->count;
}

static void mark(struct fplinux_ui_listview *list, size_t index, bool marked)
{
	lv_obj_t *row = row_at(list, index);
	lv_obj_t *marker = lv_obj_get_child(row, 0);
	bool changed = lv_obj_has_state(marker, ROW_MARKED) != marked;

	lv_obj_set_state(row, ROW_MARKED, marked);
	lv_obj_set_state(marker, ROW_MARKED, marked);
	/* The draw callback changes ink without changing the marker's styles. */
	if (changed)
		lv_obj_invalidate(marker);
}

/*
 * A plain list has no marks. A radio mark only moves: marking a row unmarks
 * the previous one, and unmarking is ignored.
 */
void fplinux_ui_listview_set_marked(struct fplinux_ui_listview *list,
				    size_t index, bool marked)
{
	if (list->is_virtual || index >= list->count ||
	    list->marker == FPLINUX_UI_LISTVIEW_PLAIN)
		return;
	if (list->marker == FPLINUX_UI_LISTVIEW_RADIO) {
		if (!marked || index == list->radio)
			return;
		if (list->radio != SIZE_MAX)
			mark(list, list->radio, false);
		list->radio = index;
	}
	mark(list, index, marked);
}

/* A virtual row shows its mark only while it is in the pool. */
bool fplinux_ui_listview_is_marked(const struct fplinux_ui_listview *list,
				   size_t index)
{
	lv_obj_t *row = index < list->count ? row_at(list, index) : NULL;

	return row && lv_obj_has_state(row, ROW_MARKED);
}

size_t fplinux_ui_listview_selected(const struct fplinux_ui_listview *list)
{
	return list->count ? list->selected : SIZE_MAX;
}

size_t fplinux_ui_listview_first_visible(const struct fplinux_ui_listview *list)
{
	int32_t top, height;
	size_t first;

	if (!list->count)
		return SIZE_MAX;
	lv_obj_update_layout(list->object);
	top = lv_obj_get_scroll_y(list->object);
	if (top < 0)
		top = 0;
	height = lv_obj_get_content_height(list->object);
	first = (size_t)(top / list->pitch);
	if (top % list->pitch && height >= 2 * list->pitch - top % list->pitch)
		++first;
	return first < list->count ? first : list->count - 1;
}

lv_obj_t *fplinux_ui_listview_selected_row(struct fplinux_ui_listview *list)
{
	return list->count ? row_at(list, list->selected) : NULL;
}

static void virtual_focus(struct fplinux_ui_listview *list, size_t index);

/* Runs the select callback when the selection moves to another row. */
void fplinux_ui_listview_select(struct fplinux_ui_listview *list, size_t index)
{
	if (index >= list->count || index == list->selected)
		return;
	if (list->is_virtual) {
		virtual_focus(list, index);
	} else if (!list->selectable) {
		list->selected = index;
		return;
	} else {
		move_selection(list, index);
	}
	if (list->select)
		list->select(list, index, list->context);
}

void fplinux_ui_listview_set_selectable(struct fplinux_ui_listview *list,
					bool selectable)
{
	if (list->selectable == selectable)
		return;
	list->selectable = selectable;
	if (list->count)
		show_selected(list, selectable);
}

void fplinux_ui_listview_set_callbacks(struct fplinux_ui_listview *list,
				       fplinux_ui_listview_cb select,
				       fplinux_ui_listview_cb activate,
				       void *context)
{
	list->select = select;
	list->activate = activate;
	list->context = context;
}

static bool overflows(lv_obj_t *object)
{
	lv_obj_update_layout(object);
	return lv_obj_get_scroll_top(object) > 0 ||
	       lv_obj_get_scroll_bottom(object) > 0;
}

static bool is_ok(unsigned int code)
{
	return code == KEY_OK || code == KEY_ENTER || code == KEY_KPENTER;
}

/* Rows wholly visible at once, at least one. */
static size_t page_rows(struct fplinux_ui_listview *list)
{
	int32_t view;

	lv_obj_update_layout(list->object);
	view = lv_obj_get_content_height(list->object);
	return view > list->pitch ? (size_t)(view / list->pitch) : 1;
}

/*
 * Moves the selection, or scrolls a read-only list, by rows: one for Up
 * and Down, a visible page for Left and Right.
 */
static bool move(struct fplinux_ui_listview *list, long rows,
		 enum fplinux_ui_focus_boundary boundary)
{
	size_t index;

	if (!list->count)
		return false;
	if (list->is_virtual) {
		virtual_focus(list,
			      fplinux_ui_focus_step(list->selected, list->count,
						    rows, boundary));
		return true;
	}
	if (!list->selectable) {
		if (!overflows(list->object))
			return false;
		lv_obj_scroll_by_bounded(list->object, 0,
					 (int32_t)-rows * list->pitch,
					 LV_ANIM_OFF);
		return true;
	}
	index = fplinux_ui_focus_step(list->selected, list->count, rows,
				      boundary);
	if (index != list->selected)
		move_selection(list, index);
	return true;
}

/*
 * Up, Down, Left and Right are used at either end too; OK is used only by
 * a selectable list with an activate callback. Callbacks run last, so that
 * they may delete the list.
 */
bool fplinux_ui_listview_key(struct fplinux_ui_listview *list,
			     const struct fplinux_ui_key *key)
{
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;
	unsigned char bit = (unsigned char)(1U << (key->code % 8));
	size_t before = list->selected;
	unsigned char *used;
	long rows;

	if (key->code >= KEY_CNT)
		return false;
	used = &list->consumed[keyboard][key->code / 8];
	if (!key->pressed) {
		bool taken = *used & bit;

		*used &= (unsigned char)~bit;
		return taken;
	}
	if (key->control || key->alt)
		return false;
	if (key->code == KEY_UP || key->code == KEY_DOWN ||
	    key->code == KEY_LEFT || key->code == KEY_RIGHT) {
		bool row_step = key->code == KEY_UP || key->code == KEY_DOWN;

		rows = row_step ? 1 : (long)page_rows(list);
		if (key->code == KEY_UP || key->code == KEY_LEFT)
			rows = -rows;
		if (!move(list, rows,
			  row_step ? FPLINUX_UI_FOCUS_WRAP :
				     FPLINUX_UI_FOCUS_CLAMP))
			return false;
		*used |= bit;
		if (list->selected != before && list->select)
			list->select(list, list->selected, list->context);
		return true;
	}
	if (is_ok(key->code) && list->selectable && list->count &&
	    list->activate) {
		*used |= bit;
		list->activate(list, list->selected, list->context);
		return true;
	}
	return false;
}

/* Virtual lists */

/* Asks for the row of slot's data index and shows it there. */
static void bind_slot(struct fplinux_ui_listview *list, size_t slot)
{
	size_t index = list->window.bound[slot];
	lv_obj_t *row = list->slots[slot];
	struct fplinux_ui_virtual_row data = { 0 };
	lv_draw_buf_t *icon = NULL;
	lv_obj_t *text = row_text(list, row);
	bool focused = index == list->window.focus;

	if (index == SIZE_MAX) {
		fplinux_ui_marquee_blur(list->ui, text);
		lv_obj_set_hidden(row, true);
		if (list->icons)
			show_icon(list, row, NULL, FPLINUX_UI_LIST_ICON_IMAGE);
		return;
	}
	if (!list->row_cb(list, index, &data, list->row_context) || !data.text)
		data = (struct fplinux_ui_virtual_row){ .text = "" };
	if (data.icon && (data.icon_kind == FPLINUX_UI_LIST_ICON_IMAGE ||
			  data.icon_kind == FPLINUX_UI_LIST_ICON_APP_TILE)) {
		int size = list->spec->row_icon_size;

		icon = fplinux_ui_image_decode(data.icon, size, size, false);
		if (icon && !list->icons)
			use_icons(list);
	}
	lv_obj_set_hidden(row, false);
	lv_obj_set_y(row, (int32_t)index * list->pitch);
	lv_label_set_text(text, data.text);
	set_detail(list, row, data.detail);
	if (list->icons)
		show_icon(list, row, icon, data.icon_kind);
	if (list->marker != FPLINUX_UI_LISTVIEW_PLAIN)
		mark(list, index, data.marked);
	set_row_state(list, row, focused && list->selectable);
	if (!focused)
		fplinux_ui_marquee_blur(list->ui, row_text(list, row));
}

/* The pool holds window.slots rows; extra ones are deleted. */
static void size_pool(struct fplinux_ui_listview *list)
{
	while (list->slot_count < list->window.slots) {
		lv_obj_t *row = make_row(list, "");

		lv_obj_set_hidden(row, true);
		list->slots[list->slot_count++] = row;
	}
	while (list->slot_count > list->window.slots) {
		lv_obj_t *row = list->slots[--list->slot_count];

		fplinux_ui_marquee_blur(list->ui, row_text(list, row));
		lv_obj_delete(row);
	}
}

static void apply_window(struct fplinux_ui_listview *list, size_t *rebind,
			 size_t count);

/* Fills the listed slots; a first detail switches the pitch once. */
static void bind_slots(struct fplinux_ui_listview *list, const size_t *rebind,
		       size_t count)
{
	bool details = list->details;

	size_pool(list);
	for (size_t i = 0; i < count; ++i)
		if (rebind[i] < list->slot_count)
			bind_slot(list, rebind[i]);
	if (list->details && !details) {
		size_t again[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
		size_t n = fplinux_ui_virtual_resize(&list->window, list->pitch,
						     list->window.view, again);

		apply_window(list, again, n);
	}
}

/*
 * Content height, scroll position, slots and the focused row's marquee.
 * Before its start the list keeps only its count and focus.
 */
static void apply_window(struct fplinux_ui_listview *list, size_t *rebind,
			 size_t count)
{
	const struct fplinux_ui_virtual_window *window = &list->window;

	list->count = window->count;
	list->selected = window->focus == SIZE_MAX ? 0 : window->focus;
	lv_obj_set_height(list->spacer, (int32_t)window->count * window->pitch);
	if (!list->started)
		return;
	bind_slots(list, rebind, count);
	/* A count change can make an unchanged pooled row the last one. */
	for (size_t slot = 0; slot < list->slot_count; ++slot) {
		size_t index = window->bound[slot];

		lv_obj_set_style_border_opa(
			list->slots[slot],
			index < list->count && index + 1 < list->count ?
				LV_OPA_COVER :
				LV_OPA_TRANSP,
			0);
	}
	lv_obj_update_layout(list->object);
	lv_obj_scroll_to_y(list->object, window->top, LV_ANIM_OFF);
	if (window->focus != SIZE_MAX)
		show_selected(list, list->selectable);
}

/*
 * The first activation fills every slot, so that the row callback never
 * runs before its screen exists for the application.
 */
static void start_virtual(struct fplinux_ui_listview *list)
{
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];

	list->started = true;
	for (size_t slot = 0; slot < list->window.slots; ++slot)
		rebind[slot] = slot;
	apply_window(list, rebind, list->window.slots);
}

static void virtual_focus(struct fplinux_ui_listview *list, size_t index)
{
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	size_t count;

	if (index == SIZE_MAX || index == list->window.focus)
		return;
	show_selected(list, false);
	count = fplinux_ui_virtual_set_focus(&list->window, index, rebind);
	apply_window(list, rebind, count);
}

static int32_t view_height(struct fplinux_ui_listview *list)
{
	lv_obj_update_layout(list->object);
	return lv_obj_get_content_height(list->object);
}

/* A new title or surface size changes the rows in view. */
static void size_changed_cb(lv_event_t *event)
{
	struct fplinux_ui_listview *list = lv_event_get_user_data(event);
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	int32_t view = lv_obj_get_content_height(list->object);
	size_t count;

	if (view == list->window.view)
		return;
	count = fplinux_ui_virtual_resize(&list->window, list->pitch, view,
					  rebind);
	apply_window(list, rebind, count);
}

struct fplinux_ui_listview *
fplinux_ui_listview_create_virtual(struct fplinux_ui *ui, lv_obj_t *parent,
				   enum fplinux_ui_listview_marker marker,
				   size_t count, fplinux_ui_listview_row_cb row,
				   void *context)
{
	struct fplinux_ui_listview *list;
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];

	if (!row) {
		errno = EINVAL;
		return NULL;
	}
	list = fplinux_ui_listview_create(ui, parent, marker);
	if (!list)
		return NULL;
	list->is_virtual = true;
	/* Its screen activates it once on top; that starts it. */
	list->active = false;
	list->row_cb = row;
	list->row_context = context;
	list->spacer = lv_obj_create(list->object);
	lv_obj_remove_style_all(list->spacer);
	lv_obj_set_scrollable(list->spacer, false);
	lv_obj_set_width(list->spacer, 1);
	fplinux_ui_virtual_init(&list->window, count, list->pitch,
				view_height(list), rebind);
	apply_window(list, rebind, 0);
	lv_obj_add_event_cb(list->object, size_changed_cb,
			    LV_EVENT_SIZE_CHANGED, list);
	return list;
}

bool fplinux_ui_listview_is_virtual(const struct fplinux_ui_listview *list)
{
	return list->is_virtual;
}

void fplinux_ui_listview_invalidate(struct fplinux_ui_listview *list,
				    size_t first, size_t count)
{
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	size_t rows;

	if (!list->is_virtual)
		return;
	rows = fplinux_ui_virtual_invalidate(&list->window, first, count,
					     rebind);
	apply_window(list, rebind, rows);
}

void fplinux_ui_listview_rows_inserted(struct fplinux_ui_listview *list,
				       size_t index, size_t count)
{
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	size_t rows;

	if (!list->is_virtual || !count)
		return;
	show_selected(list, false);
	rows = fplinux_ui_virtual_inserted(&list->window, index, count, rebind);
	apply_window(list, rebind, rows);
}

void fplinux_ui_listview_rows_deleted(struct fplinux_ui_listview *list,
				      size_t index, size_t count)
{
	size_t rebind[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
	size_t rows;

	if (!list->is_virtual || !count)
		return;
	show_selected(list, false);
	rows = fplinux_ui_virtual_deleted(&list->window, index, count, rebind);
	apply_window(list, rebind, rows);
}
