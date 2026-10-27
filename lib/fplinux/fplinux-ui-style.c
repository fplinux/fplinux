/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-internal.h"
#include "fplinux-ui-screen.h"

#include <stdlib.h>

struct scrollbar_layout {
	lv_obj_t *object;
	lv_display_t *display;
	int32_t padding;
	int32_t space;
	int32_t reserved;
	lv_scrollbar_mode_t mode;
	lv_dir_t direction;
	bool pending;
	bool changing;
};

static void scrollbar_event(lv_event_t *event);
static void scrollbar_refresh(lv_event_t *event);

static struct scrollbar_layout *scrollbar_layout(lv_obj_t *object)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
		lv_event_dsc_t *event = lv_obj_get_event_dsc(object, i);

		if (lv_event_dsc_get_cb(event) == scrollbar_event)
			return lv_event_dsc_get_user_data(event);
	}
	return NULL;
}

/* The two layouts finish before LVGL draws any of this refresh's pixels. */
void fplinux_ui_scrollbar_layout(lv_obj_t *object)
{
	struct scrollbar_layout *layout = scrollbar_layout(object);
	lv_scrollbar_mode_t mode;
	lv_dir_t direction;
	int32_t reserved;

	if (!layout || layout->changing)
		return;
	/* This also delivers deferred child size changes to the viewport. */
	lv_obj_update_layout(object);
	mode = lv_obj_get_scrollbar_mode(object);
	direction = lv_obj_get_scroll_dir(object);
	/* These setters invalidate drawing without a layout event. */
	if (mode != layout->mode || direction != layout->direction) {
		layout->mode = mode;
		layout->direction = direction;
		layout->pending = true;
	}
	if (!layout->pending)
		return;
	layout->changing = true;
	lv_obj_set_style_pad_right(object, layout->padding, 0);
	lv_obj_update_layout(object);
	reserved = mode != LV_SCROLLBAR_MODE_OFF && (direction & LV_DIR_VER) &&
				   lv_obj_get_scroll_top(object) +
						   lv_obj_get_scroll_bottom(
							   object) >
					   0 ?
			   layout->space :
			   0;
	lv_obj_set_style_pad_right(object, layout->padding + reserved, 0);
	lv_obj_update_layout(object);
	layout->reserved = reserved;
	layout->pending = false;
	layout->changing = false;
}

static void scrollbar_refresh(lv_event_t *event)
{
	struct scrollbar_layout *layout = lv_event_get_user_data(event);

	fplinux_ui_scrollbar_layout(layout->object);
}

static void scrollbar_event(lv_event_t *event)
{
	struct scrollbar_layout *layout = lv_event_get_user_data(event);
	lv_event_code_t code = lv_event_get_code(event);
	int32_t padding;

	if (code == LV_EVENT_DELETE) {
		lv_display_remove_event_cb_with_user_data(
			layout->display, scrollbar_refresh, layout);
		lv_obj_remove_event_cb_with_user_data(layout->object,
						      scrollbar_event, layout);
		free(layout);
		return;
	}
	if (layout->changing)
		return;
	if (code == LV_EVENT_STYLE_CHANGED) {
		padding = lv_obj_get_style_pad_right(layout->object, 0);
		if (padding != layout->padding + layout->reserved) {
			layout->padding = padding;
			layout->reserved = 0;
		}
	}
	if (code == LV_EVENT_STYLE_CHANGED || code == LV_EVENT_SIZE_CHANGED ||
	    code == LV_EVENT_CHILD_CHANGED || code == LV_EVENT_CHILD_CREATED ||
	    code == LV_EVENT_CHILD_DELETED || code == LV_EVENT_LAYOUT_CHANGED ||
	    code == LV_EVENT_VALUE_CHANGED)
		layout->pending = true;
}

lv_color_t fplinux_ui_alert_color(const struct fplinux_ui *ui,
				  enum fplinux_ui_alert_kind kind)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);

	switch (kind) {
	case FPLINUX_UI_ALERT_WARNING:
		return theme->warning;
	case FPLINUX_UI_ALERT_ERROR:
		return theme->danger;
	default:
		return theme->focus;
	}
}

void fplinux_ui_style_scrollbar(struct fplinux_ui *ui, lv_obj_t *object)
{
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);
	struct scrollbar_layout *layout;
	lv_border_side_t sides = lv_obj_get_style_border_side(object, 0);
	int32_t border = lv_obj_get_style_border_width(object, 0);
	int32_t inset = spec->scrollbar_offset;

	lv_obj_set_style_bg_color(object, fplinux_ui_theme(ui)->focus,
				  LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(object, 0, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(object, spec->scrollbar_width,
			       LV_PART_SCROLLBAR);
	/* LVGL positions the indicator from the outer edge, including borders. */
	lv_obj_set_style_pad_right(
		object, inset + ((sides & LV_BORDER_SIDE_RIGHT) ? border : 0),
		LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_top(
		object, inset + ((sides & LV_BORDER_SIDE_TOP) ? border : 0),
		LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_bottom(
		object, inset + ((sides & LV_BORDER_SIDE_BOTTOM) ? border : 0),
		LV_PART_SCROLLBAR);
	layout = scrollbar_layout(object);
	if (layout) {
		layout->pending = true;
		return;
	}
	layout = calloc(1, sizeof(*layout));
	if (!layout)
		return;
	layout->object = object;
	layout->display = lv_obj_get_display(object);
	layout->padding = lv_obj_get_style_pad_right(object, 0);
	layout->space = spec->scrollbar_space;
	layout->pending = true;
	lv_obj_add_event_cb(object, scrollbar_event, LV_EVENT_ALL, layout);
	lv_display_add_event_cb(layout->display, scrollbar_refresh,
				LV_EVENT_REFR_START, layout);
}
