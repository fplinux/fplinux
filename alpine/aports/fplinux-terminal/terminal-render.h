/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_RENDER_H
#define FPLINUX_TERMINAL_RENDER_H

#include "fplinux-terminal.h"
#include "fplinux-font.h"
#include <lvgl/lvgl.h>

struct fplinux_terminal_grid {
	struct fplinux_font psf;
	lv_font_t font;
	unsigned int cell_width;
	unsigned int cell_height;
};

/* Load a 6x12 or 8x16 PSF font into an unopened grid. */
bool fplinux_terminal_grid_open(struct fplinux_terminal_grid *grid,
				const char *path);
/* Close after every object that draws the grid has been deleted. */
void fplinux_terminal_grid_close(struct fplinux_terminal_grid *grid);
void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds,
			     lv_color_t accent, lv_color_t accent_text);

#endif
