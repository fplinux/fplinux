/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_RENDER_H
#define FPLINUX_TERMINAL_RENDER_H

#include "fplinux-terminal.h"

struct fplinux_terminal_glyph {
	uint32_t codepoint;
	unsigned int index;
};

struct fplinux_terminal_font {
	unsigned char *data;
	const unsigned char *bitmap;
	struct fplinux_terminal_glyph *map;
	size_t map_count;
	unsigned int width;
	unsigned int height;
	unsigned int glyph_bytes;
	unsigned int row_bytes;
};

struct fplinux_terminal_surface {
	uint16_t *pixels;
	unsigned int width;
	unsigned int height;
	unsigned int stride_bytes;
};

bool fplinux_terminal_font_open(struct fplinux_terminal_font *font,
				const char *path);
void fplinux_terminal_font_close(struct fplinux_terminal_font *font);
void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_font *font,
			     const struct fplinux_terminal_surface *surface);

#endif
