/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_STATUS_FONT_H
#define FPLINUX_STATUS_FONT_H

#include <stdint.h>

/* Indices 0..94 are U+0020..U+007E; 95..98 are U+2190..U+2193. */
struct fplinux_status_glyph {
	uint32_t bitmap_offset;
	uint16_t advance; /* 1/16 pixel */
	uint8_t box_w, box_h;
	int8_t ofs_x, ofs_y;
};

struct fplinux_status_kern {
	uint8_t left, right;
	int16_t adjustment; /* 1/16 pixel */
};

struct fplinux_status_font {
	/* Each glyph begins at a byte; pixels are packed high nibble first. */
	const uint8_t *bitmap;
	const struct fplinux_status_glyph *glyphs;
	const struct fplinux_status_kern *kerning; /* Sorted by (left, right). */
	uint16_t kerning_count;
	uint8_t line_height;
	int8_t base_line;
};

/* The returned font has static lifetime; unknown ids return NULL. */
const struct fplinux_status_font *fplinux_status_font_by_id(const char *id);

#endif
