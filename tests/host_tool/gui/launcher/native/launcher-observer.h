/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_OBSERVER_H
#define FPLINUX_LAUNCHER_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

struct launcher_line_observation {
	int bounds[4];
	int parent_bounds[4];
	int parent_radius;
	unsigned int points;
	bool visible;
};

struct launcher_label_observation {
	char text[256];
	int x;
	int y;
	int width;
	int height;
	int text_width;
	int text_height;
};

struct launcher_frame_observation {
	struct launcher_label_observation labels[32];
	unsigned int label_count;
	unsigned int clock_callbacks;
	unsigned int clock_period_ms;
	unsigned int lines;
	struct launcher_line_observation line_objects[8];
	unsigned int line_objects_count;
	uint32_t active_serial;
	unsigned int animations;
	int scroll_extent;
	bool timer_only;
	bool clock_paused;
};

#endif
