/* SPDX-License-Identifier: GPL-2.0-only */
#include "list-values-observer.h"

#include <assert.h>
#include <lvgl/lvgl.h>
#include <string.h>

static unsigned int count_images(lv_obj_t *object)
{
	unsigned int images = lv_obj_check_type(object, &lv_image_class);

	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		images +=
			count_images(lv_obj_get_child(object, (int32_t)index));
	return images;
}

void screen_observe_list_value(lv_obj_t *label,
			       struct list_value_observation *observation)
{
	lv_obj_t *parent = lv_obj_get_parent(label);
	lv_obj_t *row;
	lv_area_t bounds;

	if (!parent)
		return;
	row = lv_obj_get_style_flex_flow(parent, LV_PART_MAIN) ==
			      LV_FLEX_FLOW_ROW ?
		      parent :
		      lv_obj_get_parent(parent);
	if (!row)
		return;
	for (uint32_t index = 0; index < lv_obj_get_child_count(parent);
	     ++index) {
		lv_obj_t *detail = lv_obj_get_child(parent, (int32_t)index);
		lv_point_t natural;
		const char *value;

		if (detail == label ||
		    !lv_obj_check_type(detail, &lv_label_class))
			continue;
		value = lv_label_get_text(detail);
		assert(strlen(value) < sizeof(observation->value));
		strcpy(observation->value, value);
		lv_text_get_size(&natural, value,
				 lv_obj_get_style_text_font(detail,
							    LV_PART_MAIN),
				 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		observation->value_text_width = natural.x;
		observation->value_scrolling = lv_label_get_long_mode(detail) ==
					       LV_LABEL_LONG_MODE_SCROLL;
		lv_obj_get_coords(detail, &bounds);
		observation->value_left = bounds.x1;
		observation->value_right = bounds.x2;
		observation->value_top = bounds.y1;
		observation->value_bottom = bounds.y2;
		lv_obj_get_coords(label, &bounds);
		observation->text_left = bounds.x1;
		observation->text_right = bounds.x2;
		observation->text_top = bounds.y1;
		observation->text_bottom = bounds.y2;
		lv_obj_get_coords(row, &bounds);
		observation->row_left = bounds.x1;
		observation->row_right = bounds.x2;
		observation->row_top = bounds.y1;
		observation->row_bottom = bounds.y2;
		observation->images = count_images(row);
		observation->found = true;
		return;
	}
}
