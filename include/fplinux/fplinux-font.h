/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FONT_H
#define FPLINUX_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_font_map_entry;

struct fplinux_font {
	unsigned char *data;
	const unsigned char *bitmap;
	struct fplinux_font_map_entry *map;
	size_t map_count;
	unsigned int width;
	unsigned int height;
	unsigned int glyph_bytes;
	unsigned int row_bytes;
};

/* Initialize an unopened font from a PSF2 Unicode file, at most 16x32 and 1 MiB. */
bool fplinux_font_open(struct fplinux_font *font, const char *path);
/* Load the installed default, requiring 6x12 below 200 display pixels or 8x16. */
bool fplinux_font_open_default(struct fplinux_font *font,
			       unsigned int display_width, char *error,
			       size_t error_size);
/* Release a loaded font; zero-initialized and failed opens are safe to close. */
void fplinux_font_close(struct fplinux_font *font);
/*
 * Return MSB-first bitmap rows, falling back to U+FFFD when the codepoint is
 * absent. NULL means neither glyph is present. The pointer lives until close.
 */
const unsigned char *fplinux_font_glyph(const struct fplinux_font *font,
					uint32_t codepoint);

#endif
