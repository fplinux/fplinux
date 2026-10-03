/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_RENDER_H
#define FPLINUX_TERMINAL_RENDER_H

#include "fplinux-terminal.h"
#include "fplinux-font.h"

struct fplinux_terminal_surface {
	uint16_t *pixels;
	unsigned int width;
	unsigned int height;
	unsigned int stride_bytes;
};

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_font *font,
			     const struct fplinux_terminal_surface *surface);

#endif
