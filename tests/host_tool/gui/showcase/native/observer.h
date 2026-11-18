/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SHOWCASE_OBSERVER_H
#define FPLINUX_SHOWCASE_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct showcase_control {
	uint64_t now_ms;
	uint64_t advance_ns;
	unsigned int present_permits;
	bool measured_clock;
	bool render_timing;
	bool gpu_failure;
};

enum showcase_observation_kind {
	SHOWCASE_PAINT,
	SHOWCASE_PRESENT_REQUEST,
	SHOWCASE_RENDER,
	SHOWCASE_CLOSE,
};

struct showcase_observation {
	enum showcase_observation_kind kind;
	unsigned int paints;
	unsigned int images;
	unsigned int rendered;
	unsigned int created;
	unsigned int destroyed;
	unsigned int frame;
	unsigned int width;
	unsigned int height;
	unsigned int mode;
	unsigned int actual_mode;
	size_t focus;
	size_t first_visible;
	size_t rows;
	unsigned int chosen;
	int frame_x;
	int frame_y;
	int frame_width;
	int frame_height;
	int setup_y[4];
	uint64_t render_ns;
	bool canvas;
	bool scene_visible;
	bool fullscreen;
	char labels[8192];
};

#endif
