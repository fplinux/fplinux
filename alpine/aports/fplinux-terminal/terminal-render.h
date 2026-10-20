/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_RENDER_H
#define FPLINUX_TERMINAL_RENDER_H

#include "fplinux-terminal.h"
#include <lvgl/lvgl.h>

struct fplinux_terminal_grid {
	const lv_font_t *font;
	unsigned int cell_width;
	unsigned int cell_height;
};

/* Place the shared proportional GUI font in fixed terminal columns. */
bool fplinux_terminal_grid_init(struct fplinux_terminal_grid *grid,
				const lv_font_t *font);
void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds);

#endif
