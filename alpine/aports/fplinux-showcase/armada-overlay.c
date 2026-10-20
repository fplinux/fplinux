// SPDX-License-Identifier: GPL-2.0-only
#include "armada-overlay.h"

#include <stdio.h>
#include <string.h>

#define ARMADA_FINAL_METRICS (32U * ARMADA_FRAMES_PER_SECOND)

struct armada_overlay {
	unsigned int width;
	unsigned int height;
	uint16_t *pixels;
	size_t stride;
	const struct fplinux_font *font;
};

static void draw_character(const struct armada_overlay *overlay, int x, int y,
			   char character, uint16_t color)
{
	const struct fplinux_font *font = overlay->font;
	const unsigned char *glyph;
	unsigned int row;

	if (character >= 'a' && character <= 'z')
		character = (char)(character - 'a' + 'A');
	glyph = fplinux_font_glyph(font, (unsigned char)character);
	if (!glyph)
		return;
	for (row = 0; row < font->height; ++row) {
		unsigned int column;

		for (column = 0; column < font->width; ++column) {
			int pixel_x = x + (int)column;
			int pixel_y = y + (int)row;

			if (pixel_x >= 0 && pixel_y >= 0 &&
			    (unsigned int)pixel_x < overlay->width &&
			    (unsigned int)pixel_y < overlay->height &&
			    (glyph[row * font->row_bytes + column / 8U] &
			     (0x80U >> (column % 8U)))) {
				uint16_t *destination =
					(uint16_t *)((uint8_t *)overlay->pixels +
						     (size_t)pixel_y *
							     overlay->stride);

				destination[(unsigned int)pixel_x] = color;
			}
		}
	}
}

static void draw_centered(const struct armada_overlay *overlay, int y,
			  const char *text, uint16_t color)
{
	int text_width = (int)(strlen(text) * overlay->font->width);
	int x = ((int)overlay->width - text_width) / 2;

	while (*text) {
		draw_character(overlay, x, y, *text, color);
		x += (int)overlay->font->width;
		++text;
	}
}

void armada_overlay_draw(unsigned int width, unsigned int height,
			 uint16_t *pixels, const struct fplinux_font *font,
			 uint32_t frame, const struct armada_metrics *metrics)
{
	armada_overlay_draw_stride(width, height, pixels,
				   (size_t)width * sizeof(*pixels), font, frame,
				   metrics);
}

void armada_overlay_draw_stride(unsigned int width, unsigned int height,
				uint16_t *pixels, size_t stride,
				const struct fplinux_font *font, uint32_t frame,
				const struct armada_metrics *metrics)
{
	const struct armada_overlay overlay = {
		.width = width,
		.height = height,
		.pixels = pixels,
		.stride = stride,
		.font = font,
	};
	unsigned int line_height = font->height + 2U;
	const uint16_t cyan = 0x3f1f;
	const uint16_t white = 0xe7bf;
	char line[48];

	draw_centered(&overlay, (int)height / 12, "FPLINUX ARMADA", white);
	if (frame >= ARMADA_FINAL_METRICS && metrics && metrics->valid) {
		snprintf(line, sizeof(line), "AVG %u.%u FPS",
			 metrics->average_fps_tenths / 10U,
			 metrics->average_fps_tenths % 10U);
		draw_centered(&overlay, (int)height * 3 / 4, line, cyan);
		snprintf(line, sizeof(line), "MIN %u.%u  MAX %u MS",
			 metrics->minimum_fps_tenths / 10U,
			 metrics->minimum_fps_tenths % 10U,
			 metrics->maximum_frame_us / 1000U);
		draw_centered(&overlay, (int)height * 3 / 4 + (int)line_height,
			      line, white);
		draw_centered(&overlay, (int)height - (int)line_height,
			      "BACK EXITS", white);
	} else {
		draw_centered(&overlay, (int)height - (int)line_height,
			      "DISPLAY KEYS HAPTICS", cyan);
	}
}
