// SPDX-License-Identifier: GPL-2.0-only
#include "boot-screen.h"
#include "font.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Solid synthetic glyphs make text extents observable without a font fixture. */
#if FPLINUX_BOOT_FONT == 6
#define GLYPH_WIDTH 6U
#define GLYPH_HEIGHT 8U
#else
#define GLYPH_WIDTH 7U
#define GLYPH_HEIGHT 14U
#endif

static unsigned char glyphs[256][GLYPH_HEIGHT];
#if FPLINUX_BOOT_FONT == 6
const struct font_desc font_6x8 = {
#else
const struct font_desc font_7x14 = {
#endif
	.width = GLYPH_WIDTH,
	.height = GLYPH_HEIGHT,
	.charcount = 256,
	.data = glyphs,
};

struct canvas {
	uint32_t width;
	uint32_t height;
	uint16_t pixels[320 * 320];
	unsigned int presentations;
};

static void paint(void *context, uint32_t x, uint32_t y, uint32_t width,
		  uint32_t height, uint16_t colour)
{
	struct canvas *canvas = context;
	uint32_t row;
	uint32_t column;

	assert(x < canvas->width && width <= canvas->width - x);
	assert(y < canvas->height && height <= canvas->height - y);
	for (row = y; row < y + height; ++row)
		for (column = x; column < x + width; ++column)
			canvas->pixels[row * canvas->width + column] = colour;
}

static void present(void *context)
{
	struct canvas *canvas = context;

	++canvas->presentations;
}

int main(int argc, char **argv)
{
	const char *const stages[] = { "A", "A" };
	const struct fplinux_boot_screen_identity identity = {
		.brand = "A",
		.model = "A",
		.mode = "A",
	};
	struct canvas pixels = { 0 };
	struct fplinux_boot_screen_canvas canvas;
	struct fplinux_boot_screen screen;
	FILE *output;
	unsigned int code;

	if (argc != 5)
		return 2;
	pixels.width = (uint32_t)strtoul(argv[1], NULL, 10);
	pixels.height = (uint32_t)strtoul(argv[2], NULL, 10);
	if (pixels.width > 320 || pixels.height > 320)
		return 2;
	for (code = 33; code <= 126; ++code)
		memset(glyphs[code], GLYPH_WIDTH == 6 ? 0xfc : 0xfe,
		       GLYPH_HEIGHT);
	canvas = (struct fplinux_boot_screen_canvas){
		.width = pixels.width,
		.height = pixels.height,
		.context = &pixels,
		.fill_rect = paint,
		.present = present,
	};
	assert(fplinux_boot_screen_init(&screen, &canvas, &identity, stages,
					2) == 0);
	assert(fplinux_boot_screen_set_stage(&screen, 0,
					     FPLINUX_BOOT_SCREEN_DONE) == 0);
	assert(fplinux_boot_screen_set_checkpoint(
		       &screen, "A", FPLINUX_BOOT_SCREEN_ACTIVE) == 0);
	assert(fplinux_boot_screen_set_note(&screen, "A") == 0);
	if (strcmp(argv[3], "error") == 0)
		fplinux_boot_screen_fail(
			&screen, 12,
			"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	assert(pixels.presentations != 0);
	output = fopen(argv[4], "wb");
	if (output == NULL)
		return 1;
	if (fwrite(pixels.pixels, sizeof(*pixels.pixels),
		   pixels.width * pixels.height,
		   output) != pixels.width * pixels.height) {
		fclose(output);
		return 1;
	}
	return fclose(output) != 0;
}
