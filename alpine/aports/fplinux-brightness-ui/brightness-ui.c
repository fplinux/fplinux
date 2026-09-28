/* SPDX-License-Identifier: GPL-2.0-only */
#include "brightness-ui.h"
#include "fplinux-keypad.h"

#include <string.h>

#define COLOR_BACKGROUND 0x1082U
#define COLOR_SURFACE 0x3186U
#define COLOR_DIM 0x6b4dU
#define COLOR_TEXT 0xd6baU

struct glyph {
	char letter;
	uint8_t rows[7];
};

static const struct glyph glyphs[] = {
	{ ' ', { 0, 0, 0, 0, 0, 0, 0 } },
	{ '+', { 0, 4, 4, 31, 4, 4, 0 } },
	{ '-', { 0, 0, 0, 31, 0, 0, 0 } },
	{ '/', { 1, 1, 2, 4, 8, 16, 16 } },
	{ '0', { 14, 17, 19, 21, 25, 17, 14 } },
	{ '1', { 4, 12, 4, 4, 4, 4, 14 } },
	{ '2', { 14, 17, 1, 2, 4, 8, 31 } },
	{ '3', { 30, 1, 1, 14, 1, 1, 30 } },
	{ '4', { 2, 6, 10, 18, 31, 2, 2 } },
	{ '5', { 31, 16, 16, 30, 1, 1, 30 } },
	{ '6', { 14, 16, 16, 30, 17, 17, 14 } },
	{ '7', { 31, 1, 2, 4, 8, 8, 8 } },
	{ '8', { 14, 17, 17, 14, 17, 17, 14 } },
	{ '9', { 14, 17, 17, 15, 1, 1, 14 } },
	{ 'A', { 14, 17, 17, 31, 17, 17, 17 } },
	{ 'B', { 30, 17, 17, 30, 17, 17, 30 } },
	{ 'D', { 30, 17, 17, 17, 17, 17, 30 } },
	{ 'E', { 31, 16, 16, 30, 16, 16, 31 } },
	{ 'F', { 31, 16, 16, 30, 16, 16, 16 } },
	{ 'G', { 14, 17, 16, 23, 17, 17, 14 } },
	{ 'H', { 17, 17, 17, 31, 17, 17, 17 } },
	{ 'I', { 14, 4, 4, 4, 4, 4, 14 } },
	{ 'L', { 16, 16, 16, 16, 16, 16, 31 } },
	{ 'N', { 17, 25, 21, 19, 17, 17, 17 } },
	{ 'O', { 14, 17, 17, 17, 17, 17, 14 } },
	{ 'P', { 30, 17, 17, 30, 16, 16, 16 } },
	{ 'R', { 30, 17, 17, 30, 20, 18, 17 } },
	{ 'S', { 15, 16, 16, 14, 1, 1, 30 } },
	{ 'T', { 31, 4, 4, 4, 4, 4, 4 } },
	{ 'U', { 17, 17, 17, 17, 17, 17, 14 } },
	{ 'W', { 17, 17, 17, 21, 21, 21, 10 } },
	{ 'X', { 17, 17, 10, 4, 10, 17, 17 } },
};

enum brightness_ui_action brightness_ui_key(unsigned int level,
					    unsigned int code,
					    unsigned int *requested)
{
	if (code == FPLINUX_KEY_SOFT_RIGHT)
		return BRIGHTNESS_UI_EXIT;
	if (!requested || level > 10U)
		return BRIGHTNESS_UI_NONE;
	if (code == FPLINUX_KEY_UP && level < 10U) {
		*requested = level + 1U;
		return BRIGHTNESS_UI_SET;
	}
	if (code == FPLINUX_KEY_DOWN && level > 0U) {
		*requested = level - 1U;
		return BRIGHTNESS_UI_SET;
	}
	return BRIGHTNESS_UI_NONE;
}

static void rectangle(const struct brightness_ui_surface *surface,
		      unsigned int x, unsigned int y, unsigned int width,
		      unsigned int height, uint16_t color)
{
	unsigned int row;

	if (x >= surface->width || y >= surface->height)
		return;
	if (width > surface->width - x)
		width = surface->width - x;
	if (height > surface->height - y)
		height = surface->height - y;
	for (row = y; row < y + height; ++row) {
		uint16_t *pixels =
			(uint16_t *)((uint8_t *)surface->pixels +
				     (size_t)row * surface->stride_bytes);

		for (unsigned int column = x; column < x + width; ++column)
			pixels[column] = color;
	}
}

static const uint8_t *find_glyph(char letter)
{
	for (size_t index = 0; index < sizeof(glyphs) / sizeof(glyphs[0]);
	     ++index)
		if (glyphs[index].letter == letter)
			return glyphs[index].rows;
	return glyphs[0].rows;
}

static void label(const struct brightness_ui_surface *surface, const char *text,
		  unsigned int y, unsigned int scale, uint16_t color)
{
	size_t length = strlen(text);
	unsigned int width = (unsigned int)length * 6U * scale;
	unsigned int x =
		width < surface->width ? surface->width / 2U - width / 2U : 0U;

	for (size_t index = 0; index < length; ++index) {
		const uint8_t *glyph = find_glyph(text[index]);

		for (unsigned int row = 0; row < 7U; ++row)
			for (unsigned int column = 0; column < 5U; ++column)
				if (glyph[row] & (1U << (4U - column)))
					rectangle(surface,
						  x +
							  (unsigned int)index *
								  6U * scale +
							  column * scale,
						  y + row * scale, scale, scale,
						  color);
	}
}

bool brightness_ui_render(const struct brightness_ui_surface *surface,
			  unsigned int level, bool set_failed)
{
	char value[6] = "0/10";
	unsigned int margin;
	unsigned int bar_y;
	unsigned int bar_width;
	unsigned int segment_width;
	unsigned int gap;
	unsigned int number_scale;
	unsigned int label_scale;

	if (!surface || !surface->pixels || level > 10U ||
	    surface->width < 100U || surface->height < 130U ||
	    surface->stride_bytes < (size_t)surface->width * sizeof(uint16_t) ||
	    surface->stride_bytes % sizeof(uint16_t))
		return false;
	for (unsigned int row = 0; row < surface->height; ++row)
		rectangle(surface, 0U, row, surface->width, 1U,
			  COLOR_BACKGROUND);
	margin = surface->width / 16U;
	label_scale = surface->width >= 180U ? 2U : 1U;
	number_scale = surface->width / (level == 10U ? 31U : 25U);
	if (number_scale > surface->height / 28U)
		number_scale = surface->height / 28U;
	if (number_scale < 3U)
		number_scale = 3U;
	label(surface, "BRIGHTNESS", surface->height / 11U, 2U, COLOR_DIM);
	if (level == 10U)
		memcpy(value, "10/10", sizeof("10/10"));
	else
		value[0] = '0' + (char)level;
	label(surface, value, surface->height / 3U - 3U * number_scale,
	      number_scale, COLOR_TEXT);
	bar_y = surface->height * 62U / 100U;
	bar_width = surface->width - 2U * margin;
	gap = surface->width >= 180U ? 4U : 2U;
	segment_width = (bar_width - 9U * gap) / 10U;
	for (unsigned int segment = 0; segment < 10U; ++segment)
		rectangle(surface, margin + segment * (segment_width + gap),
			  bar_y, segment_width, surface->height / 22U,
			  segment < level ? COLOR_TEXT : COLOR_SURFACE);
	if (set_failed)
		label(surface, "SET FAILED", surface->height * 75U / 100U,
		      label_scale, COLOR_TEXT);
	else
		label(surface, "UP +  DOWN -", surface->height * 76U / 100U,
		      label_scale, COLOR_TEXT);
	label(surface, "RIGHT SOFT EXIT", surface->height * 88U / 100U,
	      label_scale, COLOR_DIM);
	return true;
}
