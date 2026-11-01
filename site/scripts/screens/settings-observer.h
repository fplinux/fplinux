/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_OBSERVER_H
#define FPLINUX_SETTINGS_OBSERVER_H

#include "list-values-observer.h"

#include <stdint.h>
#include <stdbool.h>

struct settings_frame_observation {
	unsigned int frame;
	int theme;
	uint32_t selection_mask;
	int64_t date_value;
	int64_t time_value;
	uint64_t focused_item;
	bool editing;
	bool about_memory_found;
	int about_memory_bottom;
	int about_view_bottom;
	int about_line_height;
	int about_scroll_y;
	int about_scroll_bottom;
	char locale[4];
	char labels[8192];
	struct list_value_observation root_values[4];
};

#endif
