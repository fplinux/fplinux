/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCKS_OBSERVER_H
#define FPLINUX_CLOCKS_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

struct clocks_time_observation {
	char text[32];
	int width;
	int height;
	int text_width;
	int text_height;
	int left;
	int top;
	int parent_width;
	int parent_height;
};

struct clocks_frame_observation {
	char labels[8192];
	struct clocks_time_observation time;
	unsigned int time_fields;
	unsigned int date_fields;
	unsigned int duration_fields;
	int64_t time_value;
	int64_t date_value;
	unsigned int duration_seconds;
	uint64_t focused_item;
	bool editing;
};

#endif
