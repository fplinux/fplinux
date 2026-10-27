/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-internal.h"
#include "fplinux-ui-listview.h"
#include "fplinux-ui-fonts.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The application context is peripheral to native list geometry. */
struct fplinux_ui {
	const struct fplinux_ui_profile_spec *spec;
	struct fplinux_ui_theme theme;
	lv_obj_t *text;
	lv_obj_t *detail;
};

const struct fplinux_ui_profile_spec *
fplinux_ui_spec(const struct fplinux_ui *ui)
{
	return ui->spec;
}

const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *ui)
{
	return &ui->theme;
}

void fplinux_ui_marquee_focus(struct fplinux_ui *ui, lv_obj_t *first,
			      lv_obj_t *second)
{
	ui->text = first;
	ui->detail = second;
}

void fplinux_ui_marquee_blur(struct fplinux_ui *ui, lv_obj_t *label)
{
	if (ui->text == label)
		ui->text = NULL;
	if (ui->detail == label)
		ui->detail = NULL;
}

void fplinux_ui_marquee_changed(struct fplinux_ui *ui, lv_obj_t *first,
				lv_obj_t *second)
{
	if (ui->text == first)
		ui->detail = second;
}

lv_draw_buf_t *fplinux_ui_image_decode(const void *src, int width, int height,
				       bool alpha)
{
	(void)src;
	(void)width;
	(void)height;
	(void)alpha;
	/* The virtual data in this harness contains text and details, not images. */
	assert(false);
	return NULL;
}

static unsigned int failures;
static uint8_t draw_buffer[240 * 320 * 2];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void check(bool condition, const char *description)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", description);
		++failures;
	}
}

static lv_area_t coords(lv_obj_t *object)
{
	lv_area_t area;

	lv_obj_get_coords(object, &area);
	return area;
}

static bool contains(lv_area_t outer, lv_area_t inner)
{
	return inner.x1 >= outer.x1 && inner.x2 <= outer.x2 &&
	       inner.y1 >= outer.y1 && inner.y2 <= outer.y2;
}

static bool empty(lv_area_t area)
{
	return area.x2 < area.x1 || area.y2 < area.y1;
}

static lv_obj_t *find_image(lv_obj_t *object)
{
	if (lv_obj_check_type(object, &lv_image_class))
		return object;
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index) {
		lv_obj_t *image =
			find_image(lv_obj_get_child(object, (int32_t)index));

		if (image)
			return image;
	}
	return NULL;
}

static lv_area_t row_geometry(struct fplinux_ui_listview *list, int height,
			      int padding, bool overflow)
{
	lv_obj_t *object = fplinux_ui_listview_object(list);
	lv_obj_t *selected;
	lv_area_t viewport, row;

	lv_refr_now(lv_obj_get_display(object));
	viewport = coords(object);
	selected = fplinux_ui_listview_selected_row(list);
	row = coords(selected);
	check(lv_obj_get_style_pad_top(object, 0) == 0 &&
		      lv_obj_get_style_pad_bottom(object, 0) == 0,
	      "list has no outer vertical padding");
	if (fplinux_ui_listview_selected(list) == 0)
		check(row.y1 == viewport.y1,
		      "first row starts at the viewport's top edge");
	check(row.x1 == viewport.x1, "selected row starts at the left edge");
	check(viewport.x2 - row.x2 == (overflow ? 4 : 0),
	      "selected row reserves a scrollbar lane only while overflowing");
	check(lv_area_get_height(&row) == height,
	      "selected row keeps its full pitch");
	check(lv_obj_get_style_pad_left(selected, 0) == padding &&
		      lv_obj_get_style_pad_right(selected, 0) == padding,
	      "row keeps its horizontal text padding");
	return row;
}

static void scrollbar_geometry(lv_obj_t *object, int inset, bool overflow)
{
	lv_area_t horizontal, vertical, viewport;

	lv_refr_now(lv_obj_get_display(object));
	viewport = coords(object);
	lv_obj_get_scrollbar_area(object, &horizontal, &vertical);
	check(empty(horizontal), "viewport has no horizontal scrollbar");
	check(empty(vertical) != overflow,
	      "idle thumb follows content overflow");
	check(lv_color_to_u32(lv_obj_get_style_bg_color(
		      object, LV_PART_SCROLLBAR)) == 0xff2468acU,
	      "scrollbar uses the theme focus colour");
	check(lv_obj_get_style_bg_opa(object, LV_PART_SCROLLBAR) ==
		      LV_OPA_COVER,
	      "scrollbar remains opaque while idle");
	if (!overflow || empty(vertical))
		return;
	check(contains(viewport, vertical), "thumb stays inside the viewport");
	check(lv_area_get_width(&vertical) == 2, "thumb is two pixels wide");
	check(viewport.x2 - vertical.x2 == inset,
	      "thumb has one pixel of space inside the right border");
	if (lv_obj_get_scroll_y(object) == 0)
		check(vertical.y1 - viewport.y1 == inset,
		      "thumb starts one pixel inside the top border");
	if (lv_obj_get_scroll_bottom(object) == 0)
		check(viewport.y2 - vertical.y2 == inset,
		      "thumb ends one pixel inside the bottom border");
}

static void thumb_geometry(struct fplinux_ui_listview *list, bool overflow)
{
	lv_obj_t *object = fplinux_ui_listview_object(list);
	lv_area_t horizontal, vertical;
	lv_area_t row;

	scrollbar_geometry(object, 1, overflow);
	row = coords(fplinux_ui_listview_selected_row(list));
	lv_obj_get_scrollbar_area(object, &horizontal, &vertical);
	if (overflow && !empty(vertical))
		check(vertical.x1 - row.x2 - 1 == 1,
		      "thumb has one pixel of space after the row");
}

static void check_native_viewport(struct fplinux_ui *ui, lv_obj_t *screen,
				  int width, int height, bool bordered)
{
	lv_obj_t *object = lv_obj_create(screen);
	lv_obj_t *content;

	lv_obj_remove_style_all(object);
	lv_obj_set_size(object, width, height);
	if (bordered)
		lv_obj_set_style_border_width(object, 2, 0);
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(ui, object);
	content = lv_obj_create(object);
	lv_obj_remove_style_all(content);
	lv_obj_set_size(content, 1, 1);
	scrollbar_geometry(object, bordered ? 3 : 1, false);
	lv_obj_set_height(content, height * 2);
	scrollbar_geometry(object, bordered ? 3 : 1, true);
	lv_obj_scroll_to_y(object, height / 2, LV_ANIM_OFF);
	scrollbar_geometry(object, bordered ? 3 : 1, true);
	lv_obj_scroll_to_y(object,
			   lv_obj_get_scroll_y(object) +
				   lv_obj_get_scroll_bottom(object),
			   LV_ANIM_OFF);
	check(lv_obj_get_scroll_bottom(object) == 0,
	      "native viewport reaches the bottom scroll limit");
	scrollbar_geometry(object, bordered ? 3 : 1, true);
	lv_obj_set_height(content, 1);
	lv_obj_update_layout(object);
	lv_obj_readjust_scroll(object, LV_ANIM_OFF);
	scrollbar_geometry(object, bordered ? 3 : 1, false);
	lv_obj_delete(object);
}

static void key(struct fplinux_ui_listview *list, unsigned int code)
{
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = true,
	};

	check(fplinux_ui_listview_key(list, &key), "keypad press is consumed");
	key.pressed = false;
	check(fplinux_ui_listview_key(list, &key),
	      "keypad release is consumed");
}

static bool virtual_row(struct fplinux_ui_listview *list, size_t index,
			struct fplinux_ui_virtual_row *row, void *context)
{
	(void)list;
	(void)index;
	(void)context;
	row->text = "Virtual row";
	row->detail = "Description";
	return true;
}

static void check_divider(struct fplinux_ui_listview *list, size_t index,
			  bool expected)
{
	lv_obj_t *row;
	bool has_divider;

	fplinux_ui_listview_select(list, index);
	fplinux_ui_listview_set_active(list, false);
	row = fplinux_ui_listview_selected_row(list);
	assert(row);
	has_divider = lv_obj_get_style_border_width(row, 0) > 0 &&
		      (lv_obj_get_style_border_side(row, 0) &
		       LV_BORDER_SIDE_BOTTOM) &&
		      lv_obj_get_style_border_opa(row, 0) != LV_OPA_TRANSP;
	check(has_divider == expected,
	      expected ? "non-final row has a bottom divider" :
			 "actual final row has no bottom divider");
	fplinux_ui_listview_set_active(list, true);
}

static void check_final_divider(struct fplinux_ui *ui, lv_obj_t *screen,
				bool is_virtual)
{
	struct fplinux_ui_listview *list;
	lv_obj_t *row;
	lv_area_t selected;

	if (is_virtual) {
		list = fplinux_ui_listview_create_virtual(
			ui, screen, FPLINUX_UI_LISTVIEW_PLAIN, 1, virtual_row,
			NULL);
		assert(list);
		fplinux_ui_listview_set_active(list, true);
	} else {
		list = fplinux_ui_listview_create(ui, screen,
						  FPLINUX_UI_LISTVIEW_PLAIN);
		assert(list);
		assert(fplinux_ui_listview_add(list, "First", NULL) == 0);
	}
	check_divider(list, 0, false);
	if (is_virtual)
		fplinux_ui_listview_rows_inserted(list, 1, 1);
	else
		assert(fplinux_ui_listview_add(list, "Second", NULL) == 1);
	check_divider(list, 0, true);
	check_divider(list, 1, false);
	if (is_virtual)
		fplinux_ui_listview_rows_inserted(list, 1, 1);
	else
		assert(fplinux_ui_listview_insert(list, 1, "Middle", NULL) ==
		       1);
	check_divider(list, 1, true);
	check_divider(list, 2, false);
	if (is_virtual)
		fplinux_ui_listview_rows_deleted(list, 2, 1);
	else
		assert(fplinux_ui_listview_remove(list, 2));
	check_divider(list, 1, false);
	if (is_virtual)
		fplinux_ui_listview_rows_inserted(list, 2, 38);
	else
		for (size_t index = 2; index < 40; ++index)
			assert(fplinux_ui_listview_add(list, "Another", NULL) ==
			       index);
	check_divider(list, 5, true);
	check_divider(list, 38, true);
	check_divider(list, 39, false);
	row = fplinux_ui_listview_selected_row(list);
	lv_obj_update_layout(row);
	selected = coords(row);
	check(lv_obj_get_style_bg_opa(row, 0) == LV_OPA_COVER,
	      "selected final row has an opaque background");
	check(lv_color_to_u32(lv_obj_get_style_bg_color(row, 0)) == 0xff204060U,
	      "selected final row uses the accent background");
	check(lv_area_get_height(&selected) == (is_virtual ? 32 : 22),
	      "selected final row retains its complete pitch");
	if (is_virtual)
		fplinux_ui_listview_rows_deleted(list, 1, 39);
	else {
		fplinux_ui_listview_clear(list);
		assert(fplinux_ui_listview_add(list, "Refilled", NULL) == 0);
	}
	check_divider(list, 0, false);
	lv_obj_clean(screen);
}

static void check_navigation(struct fplinux_ui *ui, lv_obj_t *screen,
			     bool is_virtual, bool one_row)
{
	struct fplinux_ui_listview *list;
	struct fplinux_ui_key press = {
		.source = FPLINUX_UI_KEY_PHONE,
		.pressed = true,
	};
	lv_obj_t *owner = lv_obj_create(screen);
	lv_obj_t *object;

	lv_obj_remove_style_all(owner);
	lv_obj_set_size(owner, lv_pct(100), lv_pct(100));
	if (is_virtual) {
		list = fplinux_ui_listview_create_virtual(
			ui, owner, FPLINUX_UI_LISTVIEW_PLAIN, 7, virtual_row,
			NULL);
		assert(list);
	} else {
		list = fplinux_ui_listview_create(ui, owner,
						  FPLINUX_UI_LISTVIEW_PLAIN);
		assert(list);
		for (size_t index = 0; index < 7; ++index)
			assert(fplinux_ui_listview_add(list, "Stored row",
						       NULL) == index);
	}
	object = fplinux_ui_listview_object(list);
	if (one_row)
		lv_obj_set_height(owner, ui->spec->row_height_detail);
	fplinux_ui_listview_set_active(list, true);
	if (!one_row) {
		press.code = KEY_UP;
		check(fplinux_ui_listview_key(list, &press),
		      "initial Up is consumed");
		check(fplinux_ui_listview_selected(list) == 6,
		      "initial Up wraps from the first row to the last");
		check(contains(coords(object),
			       coords(fplinux_ui_listview_selected_row(list))),
		      "wrapped final row is fully visible");
		fplinux_ui_listview_select(list, 0);
		check(fplinux_ui_listview_key(list, &press),
		      "held Up is consumed");
		check(fplinux_ui_listview_selected(list) == 6,
		      "held Up also wraps from the first row to the last");
		press.pressed = false;
		check(fplinux_ui_listview_key(list, &press),
		      "Up release is consumed after wrapping");
		press.pressed = true;
		press.code = KEY_DOWN;
		check(fplinux_ui_listview_key(list, &press),
		      "initial Down is consumed");
		check(fplinux_ui_listview_selected(list) == 0,
		      "initial Down wraps from the last row to the first");
		fplinux_ui_listview_select(list, 6);
		check(fplinux_ui_listview_key(list, &press),
		      "held Down is consumed");
		check(fplinux_ui_listview_selected(list) == 0,
		      "held Down also wraps from the last row to the first");
		check(contains(coords(object),
			       coords(fplinux_ui_listview_selected_row(list))),
		      "wrapped first row is fully visible");
	}
	fplinux_ui_listview_select(list, 0);
	key(list, KEY_LEFT);
	check(fplinux_ui_listview_selected(list) == 0,
	      "Left page clamps at the first row");
	key(list, KEY_RIGHT);
	check(fplinux_ui_listview_selected(list) > 0,
	      "Right page advances selection");
	if (one_row)
		check(fplinux_ui_listview_selected(list) == 1,
		      "one-row Right page advances exactly one row");
	fplinux_ui_listview_select(list, 6);
	key(list, KEY_RIGHT);
	check(fplinux_ui_listview_selected(list) == 6,
	      "Right page clamps at the final row");
	lv_obj_delete(owner);
}

static void check_contents(struct fplinux_ui *ui, lv_area_t row, bool details)
{
	check(ui->text && contains(row, coords(ui->text)),
	      "selected text stays inside the row");
	if (details)
		check(ui->detail && contains(row, coords(ui->detail)),
		      "selected detail stays inside the row");
}

static void check_first_detail(struct fplinux_ui *ui, lv_obj_t *screen,
			       const char *operation, bool selectable)
{
	struct fplinux_ui_listview *list = fplinux_ui_listview_create(
		ui, screen, FPLINUX_UI_LISTVIEW_PLAIN);
	lv_obj_t *object, *focused;
	lv_area_t before, after;

	assert(list);
	fplinux_ui_listview_set_active(list, true);
	for (size_t index = 0; index < 40; ++index)
		check(fplinux_ui_listview_add(list, "Stored row", NULL) ==
			      index,
		      "single-line stored row is added");
	fplinux_ui_listview_select(list, 39);
	object = fplinux_ui_listview_object(list);
	lv_obj_update_layout(object);
	focused = fplinux_ui_listview_selected_row(list);
	before = coords(focused);
	check(contains(coords(object), before),
	      "selected single-line row starts fully visible");
	if (!selectable) {
		fplinux_ui_listview_set_selectable(list, false);
		lv_obj_scroll_to_y(object, 0, LV_ANIM_OFF);
		lv_obj_update_layout(object);
		check(!ui->text && !ui->detail,
		      "read-only list has no focused marquee");
	}
	if (!strcmp(operation, "set"))
		check(fplinux_ui_listview_set_text(list, 0, "Updated row",
						   "First detail"),
		      "existing row accepts the first detail");
	else if (!strcmp(operation, "append"))
		check(fplinux_ui_listview_add(list, "Appended row",
					      "First detail") == 40,
		      "appended row supplies the first detail");
	else
		check(fplinux_ui_listview_insert(list, 1, "Inserted row",
						 "First detail") == 1,
		      "row inserted before focus supplies the first detail");
	lv_obj_update_layout(object);
	after = coords(fplinux_ui_listview_selected_row(list));
	check(lv_area_get_height(&after) > lv_area_get_height(&before),
	      "the first detail grows the stored rows");
	check(fplinux_ui_listview_selected(list) ==
		      (!strcmp(operation, "insert") ? 40U : 39U),
	      "selection index follows the same stored row");
	check(fplinux_ui_listview_selected_row(list) == focused,
	      "the first detail preserves the selected row identity");
	if (selectable)
		check(contains(coords(object), after),
		      "the first detail keeps the selected row fully visible");
	else {
		check(lv_obj_get_scroll_y(object) == 0,
		      "the first detail does not scroll a read-only list to selection");
		check(!ui->text && !ui->detail,
		      "the first detail does not focus a read-only list");
	}
	lv_obj_clean(screen);
}

static void check_text_update_keeps_scroll(struct fplinux_ui *ui,
					   lv_obj_t *screen)
{
	struct fplinux_ui_listview *list = fplinux_ui_listview_create(
		ui, screen, FPLINUX_UI_LISTVIEW_PLAIN);
	lv_obj_t *object;

	assert(list);
	for (size_t index = 0; index < 40; ++index)
		check(fplinux_ui_listview_add(list, "Stored row", NULL) ==
			      index,
		      "single-line stored row is added");
	fplinux_ui_listview_select(list, 39);
	object = fplinux_ui_listview_object(list);
	lv_obj_scroll_to_y(object, 0, LV_ANIM_OFF);
	lv_obj_update_layout(object);
	check(fplinux_ui_listview_set_text(list, 0, "Updated row", NULL),
	      "text-only update succeeds");
	lv_obj_update_layout(object);
	check(lv_obj_get_scroll_y(object) == 0,
	      "text-only update preserves the current scroll position");
	check(fplinux_ui_listview_selected(list) == 39,
	      "text-only update preserves selection");
	lv_obj_clean(screen);
}

static void check_first_visible(struct fplinux_ui *ui, lv_obj_t *screen,
				bool is_virtual)
{
	struct fplinux_ui_listview *list;
	lv_obj_t *object;
	int32_t pitch = is_virtual ? ui->spec->row_height_detail :
				     ui->spec->row_height;

	if (is_virtual) {
		list = fplinux_ui_listview_create_virtual(
			ui, screen, FPLINUX_UI_LISTVIEW_PLAIN, 40, virtual_row,
			NULL);
		assert(list);
		fplinux_ui_listview_set_active(list, true);
	} else {
		list = fplinux_ui_listview_create(ui, screen,
						  FPLINUX_UI_LISTVIEW_PLAIN);
		assert(list);
		check(fplinux_ui_listview_first_visible(list) == SIZE_MAX,
		      "an empty list has no visible row");
		for (size_t index = 0; index < 40; ++index)
			assert(fplinux_ui_listview_add(list, "Row", NULL) ==
			       index);
	}
	object = fplinux_ui_listview_object(list);
	lv_obj_update_layout(object);
	check(fplinux_ui_listview_first_visible(list) == 0,
	      "the initial first row is visible");
	lv_obj_scroll_to_y(object, pitch, LV_ANIM_OFF);
	check(fplinux_ui_listview_first_visible(list) == 1,
	      "an aligned scroll returns the first whole row");
	lv_obj_scroll_to_y(object, pitch + 1, LV_ANIM_OFF);
	check(fplinux_ui_listview_first_visible(list) == 2,
	      "a clipped leading row is skipped");
	lv_obj_set_height(object, pitch - 1);
	lv_obj_update_layout(object);
	lv_obj_scroll_to_y(object, pitch + 1, LV_ANIM_OFF);
	check(fplinux_ui_listview_first_visible(list) == 1,
	      "a short viewport returns its first intersecting row");
	lv_obj_set_height(object, pitch);
	lv_obj_update_layout(object);
	lv_obj_scroll_to_y(object, pitch + 1, LV_ANIM_OFF);
	check(fplinux_ui_listview_first_visible(list) == 1,
	      "an unaligned single-row viewport returns its first intersecting row");
	lv_obj_clean(screen);
}

static void status_row_geometry(lv_obj_t *row, lv_obj_t *text, lv_obj_t *image,
				const char *phase)
{
	lv_obj_t *column = lv_obj_get_parent(text);
	lv_obj_t *detail = column == row ? NULL : lv_obj_get_child(column, 1);
	lv_area_t row_bounds = coords(row), primary = coords(text);
	lv_area_t status = coords(image);

	fprintf(stderr,
		"Status row %s: row=[%d,%d..%d,%d] primary=[%d,%d..%d,%d] status=[%d,%d..%d,%d]\n",
		phase, row_bounds.x1, row_bounds.y1, row_bounds.x2,
		row_bounds.y2, primary.x1, primary.y1, primary.x2, primary.y2,
		status.x1, status.y1, status.x2, status.y2);
	check(contains(row_bounds, primary), "status primary fits its row");
	check(contains(row_bounds, status), "status image fits its row");
	check(primary.x2 < status.x1, "status image follows the primary text");
	if (detail) {
		lv_area_t secondary = coords(detail);

		fprintf(stderr, "Status detail: [%d,%d..%d,%d]\n", secondary.x1,
			secondary.y1, secondary.x2, secondary.y2);
		check(contains(row_bounds, secondary),
		      "status detail fits its row");
		check(primary.y2 < secondary.y1,
		      "status detail sits below the primary text");
		check(secondary.x2 < status.x1,
		      "status image follows the detail column");
	}
}

static void check_status_row(struct fplinux_ui *ui, lv_obj_t *screen)
{
	struct fplinux_ui_listview *list;
	lv_draw_buf_t *icon =
		lv_draw_buf_create(2, 2, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
	lv_obj_t *row, *text, *image;
	lv_area_t without, with;

	ui->theme.font = fplinux_ui_builtin_font(ui->spec->font_body);
	ui->theme.font_large = fplinux_ui_builtin_font(ui->spec->font_title);
	assert(ui->theme.font && ui->theme.font_large);
	list = fplinux_ui_listview_create(ui, screen,
					  FPLINUX_UI_LISTVIEW_PLAIN);
	assert(list && icon);
	assert(fplinux_ui_listview_add(list, "07:30", NULL) == 0);
	row = fplinux_ui_listview_selected_row(list);
	text = lv_obj_get_child(row, 0);
	lv_obj_update_layout(screen);
	without = coords(text);
	assert(fplinux_ui_listview_set_trailing_icon(list, 0, icon));
	lv_obj_update_layout(screen);
	image = find_image(row);
	assert(image);
	status_row_geometry(row, text, image, "trailing before detail");
	with = coords(text);
	assert(with.x2 < coords(image).x1 && with.x2 < without.x2);
	assert(fplinux_ui_listview_set_emphasis(list, 0, true));
	assert(lv_obj_get_style_text_font(text, 0) == ui->theme.font_large);
	/* Adding a first detail keeps the trailing image outside the text column. */
	assert(fplinux_ui_listview_set_text(list, 0, "07:30", "Wake up"));
	lv_obj_update_layout(screen);
	status_row_geometry(row, text, image, "emphasis before first detail");
	assert(coords(lv_obj_get_parent(text)).x2 < coords(image).x1);
	assert(!strcmp(
		lv_label_get_text(lv_obj_get_child(lv_obj_get_parent(text), 1)),
		"Wake up"));
	assert(fplinux_ui_listview_set_emphasis(list, 0, false));
	assert(lv_obj_get_style_text_font(text, 0) == ui->theme.font);
	assert(fplinux_ui_listview_set_trailing_icon(list, 0, NULL));
	lv_obj_update_layout(screen);
	assert(!find_image(row));
	assert(coords(lv_obj_get_parent(text)).x2 > with.x2);
	lv_obj_clean(screen);

	/* A row can also have its detail before emphasis and a status are set. */
	list = fplinux_ui_listview_create(ui, screen,
					  FPLINUX_UI_LISTVIEW_PLAIN);
	assert(list);
	assert(fplinux_ui_listview_add(list, "08:45", "Wake up") == 0);
	row = fplinux_ui_listview_selected_row(list);
	text = lv_obj_get_child(lv_obj_get_child(row, 0), 0);
	assert(fplinux_ui_listview_set_emphasis(list, 0, true));
	assert(fplinux_ui_listview_set_trailing_icon(list, 0, icon));
	lv_obj_update_layout(screen);
	image = find_image(row);
	assert(image);
	status_row_geometry(row, text, image,
			    "detail before emphasis and trailing");
	check(fplinux_ui_listview_selected_row(list) == row,
	      "status changes retain the selected row object");
	lv_obj_clean(screen);
	/* The caller retains the borrowed status image after removal and cleanup. */
	lv_draw_buf_destroy(icon);
}

int main(int argc, char **argv)
{
	struct fplinux_ui ui = { 0 };
	struct fplinux_ui_listview *list;
	lv_display_t *display;
	lv_obj_t *screen, *object;
	lv_area_t short_row, long_row, shrunk_row;
	bool is_virtual, details;
	int width, height, row_height, padding;

	if (argc != 6)
		return 2;
	width = atoi(argv[2]);
	height = atoi(argv[3]);
	row_height = atoi(argv[4]);
	padding = atoi(argv[5]);
	is_virtual = !strcmp(argv[1], "virtual");
	details = is_virtual || !strcmp(argv[1], "detail") ||
		  !strcmp(argv[1], "inline");
	lv_init();
	display = lv_display_create(width, height);
	assert(display);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	screen = lv_display_get_screen_active(display);
	lv_obj_remove_style_all(screen);
	lv_obj_set_size(screen, width, height);
	ui.spec = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	ui.theme.font = LV_FONT_DEFAULT;
	ui.theme.accent = lv_color_hex(0x204060);
	ui.theme.accent_text = lv_color_white();
	ui.theme.focus = lv_color_hex(0x2468ac);
	if (!strcmp(argv[1], "status-row")) {
		check_status_row(&ui, screen);
		goto done;
	}
	if (!strncmp(argv[1], "final-divider-", 14)) {
		check_final_divider(&ui, screen,
				    !strcmp(argv[1] + 14, "virtual"));
		goto done;
	}
	if (!strncmp(argv[1], "navigation-", 11)) {
		check_navigation(&ui, screen,
				 strstr(argv[1], "virtual") != NULL,
				 strstr(argv[1], "one-row") != NULL);
		goto done;
	}
	if (!strncmp(argv[1], "first-detail-", 13)) {
		const char *operation = argv[1] + 13;
		bool selectable = strncmp(operation, "read-only-", 10) != 0;

		check_first_detail(&ui, screen,
				   selectable ? operation : operation + 10,
				   selectable);
		goto done;
	}
	if (!strcmp(argv[1], "text-update")) {
		check_text_update_keeps_scroll(&ui, screen);
		goto done;
	}
	if (!strncmp(argv[1], "first-visible-", 14)) {
		check_first_visible(&ui, screen,
				    !strcmp(argv[1] + 14, "virtual"));
		goto done;
	}
	if (is_virtual) {
		list = fplinux_ui_listview_create_virtual(
			&ui, screen, FPLINUX_UI_LISTVIEW_PLAIN, 1, virtual_row,
			NULL);
		assert(list);
		fplinux_ui_listview_set_active(list, true);
	} else {
		list = fplinux_ui_listview_create(&ui, screen,
						  FPLINUX_UI_LISTVIEW_PLAIN);
		assert(list);
		fplinux_ui_listview_set_inline_detail(list, !strcmp(argv[1],
								    "inline"));
		check(fplinux_ui_listview_add(list, "First row",
					      details ? "Description" : NULL) ==
			      0,
		      "first row is added");
		if (!strcmp(argv[1], "icon")) {
			lv_draw_buf_t *icon = lv_draw_buf_create(
				2, 2, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);

			assert(icon);
			check(fplinux_ui_listview_set_icon(
				      list, 0, icon,
				      FPLINUX_UI_LIST_ICON_IMAGE),
			      "row accepts an owned icon");
		}
	}
	object = fplinux_ui_listview_object(list);
	short_row = row_geometry(list, row_height, padding, false);
	thumb_geometry(list, false);
	check_contents(&ui, short_row, details);
	if (!strcmp(argv[1], "icon")) {
		lv_obj_t *image =
			find_image(fplinux_ui_listview_selected_row(list));

		check(image && contains(short_row, coords(image)),
		      "row icon stays inside the selected row");
		if (image)
			check(coords(image).x2 < coords(ui.text).x1,
			      "row text starts after the icon");
	}
	if (is_virtual)
		fplinux_ui_listview_rows_inserted(list, 1, 39);
	else
		for (size_t index = 1; index < 40; ++index)
			check(fplinux_ui_listview_add(list, "Another row",
						      details ? "Detail" :
								NULL) == index,
			      "overflow row is added");
	long_row = row_geometry(list, row_height, padding, true);
	check(short_row.x1 == long_row.x1 && short_row.x2 - long_row.x2 == 4,
	      "overflow adds only the four-pixel scrollbar lane");
	thumb_geometry(list, true);
	if (!strcmp(argv[1], "scrollbar")) {
		check_native_viewport(&ui, screen, width, height, false);
		check_native_viewport(&ui, screen, width, height, true);
	}
	key(list, KEY_DOWN);
	check(fplinux_ui_listview_selected(list) == 1,
	      "Down advances selection by one row");
	for (size_t index = 1; index < 40; ++index)
		key(list, KEY_RIGHT);
	check(fplinux_ui_listview_selected(list) == 39,
	      "Right pages to the final row without wrapping");
	long_row = row_geometry(list, row_height, padding, true);
	check(contains(coords(object), long_row),
	      "keypad scrolling fully reveals the selected final row");
	check_contents(&ui, long_row, details);
	check(lv_obj_get_scroll_y(object) > 0,
	      "keypad navigation scrolls the list");
	thumb_geometry(list, true);
	key(list, KEY_UP);
	check(fplinux_ui_listview_selected(list) == 38,
	      "Up returns to the preceding row");
	if (is_virtual)
		fplinux_ui_listview_rows_deleted(list, 1, 39);
	else
		for (size_t index = 39; index > 0; --index)
			check(fplinux_ui_listview_remove(list, index),
			      "overflow row is removed");
	shrunk_row = row_geometry(list, row_height, padding, false);
	check(short_row.x1 == shrunk_row.x1 && short_row.x2 == shrunk_row.x2,
	      "shrinking restores the full row width");
	thumb_geometry(list, false);
	lv_obj_clean(screen);
done:
	lv_display_delete(display);
	lv_deinit();
	return failures ? 1 : 0;
}
