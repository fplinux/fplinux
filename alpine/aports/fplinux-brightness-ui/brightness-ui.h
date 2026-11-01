/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BRIGHTNESS_UI_H
#define FPLINUX_BRIGHTNESS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum brightness_ui_action {
	BRIGHTNESS_UI_NONE,
	BRIGHTNESS_UI_SET,
	BRIGHTNESS_UI_EXIT,
};

struct brightness_ui_surface {
	uint16_t *pixels;
	unsigned int width;
	unsigned int height;
	size_t stride_bytes;
};

enum brightness_ui_action brightness_ui_key(unsigned int level,
					    unsigned int code,
					    unsigned int *requested);
bool brightness_ui_render(const struct brightness_ui_surface *surface,
			  unsigned int level, bool set_failed);

#endif
