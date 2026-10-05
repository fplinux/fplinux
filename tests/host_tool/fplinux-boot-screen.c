// SPDX-License-Identifier: GPL-2.0-only
#include "boot-screen.h"

#include "font.h"
#include <stdio.h>

/* Synthetic font data isolates glyph indexing and drawing from upstream data. */
#if FPLINUX_BOOT_FONT == 6
static const unsigned char glyphs[256][8] = {
	['!'] = { 0x84, 0x48, 0x30, 0x30, 0x48, 0x84 },
	['?'] = { 0xfc, 0x04, 0x08, 0x10, 0x10, 0x00, 0x10 },
};
const struct font_desc font_6x8 = {
	.width = 6,
	.height = 8,
	.charcount = 256,
	.data = glyphs,
};
#else
static const unsigned char glyphs[256][14] = {
	['!'] = { 0x82, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
		  0x10, 0x28, 0x44, 0x82 },
	['?'] = { 0xfe, 0x02, 0x04, 0x08, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
		  0x00, 0x10 },
};
const struct font_desc font_7x14 = {
	.width = 7,
	.height = 14,
	.charcount = 256,
	.data = glyphs,
};
#endif

static void record_rect(void *context, uint32_t x, uint32_t y, uint32_t width,
			uint32_t height, uint16_t colour)
{
	(void)context;
	if (colour == 0xf79eU)
		printf("%u %u %u %u\n", x, y, width, height);
}

static void present(void *context)
{
	(void)context;
}

int main(int argc, char *argv[])
{
	const struct fplinux_boot_screen_identity identity = {
		.brand = argc > 1 ? argv[1] : "!",
		.model = "",
		.mode = "",
	};
	struct fplinux_boot_screen_canvas canvas = {
		.height = 160,
		.fill_rect = record_rect,
		.present = present,
	};
	struct fplinux_boot_screen screen;
	const uint32_t widths[] = { 128, 240 };
	size_t i;

	for (i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
		canvas.width = widths[i];
		printf("canvas %u\n", canvas.width);
		if (fplinux_boot_screen_init(&screen, &canvas, &identity, NULL,
					     0) != 0)
			return 1;
	}
	return 0;
}
