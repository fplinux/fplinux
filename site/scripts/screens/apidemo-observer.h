/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_APIDEMO_OBSERVER_H
#define FPLINUX_APIDEMO_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum apidemo_observation_kind {
	APIDEMO_LABEL,
	APIDEMO_LIST,
	APIDEMO_FORM,
	APIDEMO_ALERT,
	APIDEMO_CANVAS,
	APIDEMO_PREFERENCE,
	APIDEMO_TEXT_BOX,
};

struct apidemo_observation {
	enum apidemo_observation_kind kind;
	char title[128];
	char text[512];
};

struct apidemo_rectangle {
	int left;
	int top;
	int width;
	int height;
};

struct apidemo_image_observation {
	struct apidemo_rectangle bounds;
	struct apidemo_rectangle frame;
	struct apidemo_rectangle viewport;
	int source_width;
	int source_height;
};

struct apidemo_menu_observation {
	char text[128];
	struct apidemo_rectangle bounds;
	bool icon;
	int icon_width;
	int icon_height;
	uint8_t alpha[24 * 24];
};

struct apidemo_canvas_object_observation {
	int parent;
	struct apidemo_rectangle bounds;
	struct apidemo_rectangle content;
	int border;
	int padding[4];
	int scroll_y;
	int line_height;
	int max_height;
	char text[512];
};

struct apidemo_frame_observation {
	uint32_t serial;
	struct apidemo_rectangle picker;
	size_t image_count;
	struct apidemo_image_observation images[16];
	size_t menu_count;
	struct apidemo_menu_observation menu[32];
	struct apidemo_rectangle canvas;
	size_t canvas_object_count;
	struct apidemo_canvas_object_observation canvas_objects[64];
};

#endif
