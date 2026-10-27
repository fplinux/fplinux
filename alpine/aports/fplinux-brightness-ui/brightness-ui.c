/* SPDX-License-Identifier: GPL-2.0-only */
#include "brightness-ui.h"
#include "fplinux-keypad.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-session.h"

#include <string.h>

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

static void label(const struct brightness_ui_surface *surface,
		  const struct fplinux_font *font, const char *text,
		  unsigned int y, unsigned int scale, uint16_t color)
{
	size_t length = strlen(text);
	unsigned int width = (unsigned int)length * font->width * scale;
	unsigned int x =
		width < surface->width ? (surface->width - width) / 2U : 0U;

	for (size_t index = 0; index < length; ++index) {
		const unsigned char *glyph =
			fplinux_font_glyph(font, (unsigned char)text[index]);

		if (!glyph)
			continue;
		for (unsigned int row = 0; row < font->height; ++row)
			for (unsigned int column = 0; column < font->width;
			     ++column)
				if (glyph[row * font->row_bytes + column / 8U] &
				    (0x80U >> (column % 8U)))
					rectangle(surface,
						  x +
							  (unsigned int)index *
								  font->width *
								  scale +
							  column * scale,
						  y + row * scale, scale, scale,
						  color);
	}
}

bool brightness_ui_render(const struct brightness_ui_surface *surface,
			  const struct fplinux_font *font, unsigned int level,
			  bool set_failed)
{
	const struct fplinux_ui_theme_spec *theme =
		fplinux_ui_theme_spec(FPLINUX_UI_SESSION_THEME_DARK);
	uint16_t background =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_BACKGROUND);
	uint16_t unfilled =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_SURFACE);
	uint16_t muted = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_MUTED);
	uint16_t foreground =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_FOREGROUND);
	uint16_t filled = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_FOCUS);
	uint16_t danger = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_DANGER);
	char value[6] = "0/10";
	unsigned int margin;
	unsigned int bar_y;
	unsigned int bar_width;
	unsigned int bar_x;
	unsigned int segment_width;
	unsigned int gap;
	unsigned int number_scale;
	unsigned int title_scale;

	if (!surface || !surface->pixels || !font || !font->bitmap ||
	    !font->width || !font->height || level > 10U ||
	    surface->width < 100U || surface->height < 128U ||
	    surface->stride_bytes < (size_t)surface->width * sizeof(uint16_t) ||
	    surface->stride_bytes % sizeof(uint16_t))
		return false;
	for (unsigned int row = 0; row < surface->height; ++row)
		rectangle(surface, 0U, row, surface->width, 1U, background);
	margin = surface->width / 16U;
	title_scale = 20U * font->width <= surface->width ? 2U : 1U;
	number_scale =
		surface->width / ((level == 10U ? 6U : 5U) * font->width);
	if (number_scale > surface->height / (font->height * 4U))
		number_scale = surface->height / (font->height * 4U);
	if (!number_scale)
		number_scale = 1U;
	label(surface, font, "BRIGHTNESS", surface->height / 11U, title_scale,
	      muted);
	if (level == 10U)
		memcpy(value, "10/10", sizeof("10/10"));
	else
		value[0] = '0' + (char)level;
	label(surface, font, value,
	      surface->height * 2U / 5U - font->height * number_scale / 2U,
	      number_scale, foreground);
	bar_y = surface->height * 62U / 100U;
	bar_width = surface->width - 2U * margin;
	gap = surface->width >= 180U ? 4U : 2U;
	segment_width = (bar_width - 9U * gap) / 10U;
	bar_width = 10U * segment_width + 9U * gap;
	bar_x = (surface->width - bar_width) / 2U;
	for (unsigned int segment = 0; segment < 10U; ++segment)
		rectangle(surface, bar_x + segment * (segment_width + gap),
			  bar_y, segment_width, surface->height / 22U,
			  segment < level ? filled : unfilled);
	if (set_failed)
		label(surface, font, "SET FAILED", surface->height * 75U / 100U,
		      1U, danger);
	else
		label(surface, font, "UP +  DOWN -",
		      surface->height * 76U / 100U, 1U, foreground);
	label(surface, font, "RIGHT SOFT EXIT", surface->height * 88U / 100U,
	      1U, muted);
	return true;
}
