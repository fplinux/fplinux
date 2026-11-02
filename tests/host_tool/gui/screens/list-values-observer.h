/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LIST_VALUES_OBSERVER_H
#define FPLINUX_LIST_VALUES_OBSERVER_H

#include <stdbool.h>
#include <assert.h>

struct _lv_obj_t;

struct list_value_observation {
	char value[256];
	int row_left;
	int row_right;
	int row_top;
	int row_bottom;
	int text_left;
	int text_right;
	int text_top;
	int text_bottom;
	int value_left;
	int value_right;
	int value_top;
	int value_bottom;
	int value_text_width;
	unsigned int images;
	bool found;
	bool value_scrolling;
};

void screen_observe_list_value(struct _lv_obj_t *label,
			       struct list_value_observation *observation);

static inline void
screen_check_list_value(const struct list_value_observation *row,
			bool landscape)
{
	assert(row->found);
	assert(row->images == 1);
	assert(row->row_left <= row->text_left);
	assert(row->text_right <= row->row_right);
	assert(row->row_left <= row->value_left);
	assert(row->value_right <= row->row_right);
	assert(row->row_top <= row->text_top);
	assert(row->text_bottom <= row->row_bottom);
	assert(row->row_top <= row->value_top);
	assert(row->value_bottom <= row->row_bottom);
	if (landscape) {
		assert(row->text_right < row->value_left);
		assert(row->text_top == row->value_top);
	} else {
		assert(row->text_bottom < row->value_top);
		assert(row->text_left == row->value_left);
	}
}

#endif
