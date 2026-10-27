// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme Canvas text without VM, phoneME or LVGL state; see
 * fplinux-pm-glyph.h. Host tests compile this file on its own.
 */
#include "fplinux-pm-glyph.h"

#include <limits.h>
#include <stddef.h>

struct fplinux_pm_font_choice fplinux_pm_font_choose(int style, int size)
{
	struct fplinux_pm_font_choice choice = {
		.slot = FPLINUX_PM_FONT_BODY,
		.underline = (style & FPLINUX_PM_STYLE_UNDERLINED) != 0,
	};

	switch (size) {
	case FPLINUX_PM_SIZE_LARGE:
		choice.slot = FPLINUX_PM_FONT_LARGE;
		break;
	case FPLINUX_PM_SIZE_SMALL:
		choice.slot = FPLINUX_PM_FONT_SMALL;
		break;
	default:
		break;
	}
	choice.synthetic_bold = (style & FPLINUX_PM_STYLE_BOLD) != 0 &&
				choice.slot != FPLINUX_PM_FONT_LARGE;
	return choice;
}

uint32_t fplinux_pm_utf16_next(const uint16_t *chars, int n, int *index)
{
	uint32_t unit = chars[(*index)++];
	uint32_t low;

	if (unit < 0xd800 || unit > 0xdfff)
		return unit;
	if (unit > 0xdbff || *index >= n)
		return 0xfffd;
	low = chars[*index];
	if (low < 0xdc00 || low > 0xdfff)
		return 0xfffd;
	++*index;
	return 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
}

bool fplinux_pm_clip(const int16_t clip[4], int width, int height,
		     struct fplinux_pm_rect *out)
{
	out->x1 = clip[0] > 0 ? clip[0] : 0;
	out->y1 = clip[1] > 0 ? clip[1] : 0;
	out->x2 = clip[2] < width ? clip[2] : width;
	out->y2 = clip[3] < height ? clip[3] : height;
	return out->x1 < out->x2 && out->y1 < out->y2;
}

uint16_t fplinux_pm_blend(uint16_t bg, uint16_t fg, unsigned int coverage)
{
	/*
	 * The mask moves green to the upper half-word and leaves a gap of at
	 * least five bits above each field, so one multiplication by a 5-bit
	 * alpha blends all three: each field becomes
	 * bg + floor((fg - bg) * alpha / 32), the borrows of negative
	 * differences ending in the masked gaps.
	 */
	const uint32_t mask = 0x07e0f81f;
	uint32_t alpha = (coverage + 4) >> 3;
	uint32_t b = (bg | (uint32_t)bg << 16) & mask;
	uint32_t f = (fg | (uint32_t)fg << 16) & mask;
	uint32_t mix = ((((f - b) * alpha) >> 5) + b) & mask;

	return (uint16_t)(mix | mix >> 16);
}

void fplinux_pm_glyph_blend(const struct fplinux_pm_target *target,
			    const struct fplinux_pm_rect *clip, int x, int y,
			    const uint8_t *coverage, int stride, int box_w,
			    int box_h, bool bold, uint16_t color)
{
	int width = box_w + (bold ? 1 : 0);
	int col0 = clip->x1 > x ? clip->x1 - x : 0;
	int col1 = clip->x2 - x < width ? clip->x2 - x : width;
	int row0 = clip->y1 > y ? clip->y1 - y : 0;
	int row1 = clip->y2 - y < box_h ? clip->y2 - y : box_h;

	for (int row = row0; row < row1; ++row) {
		const uint8_t *line = coverage + (ptrdiff_t)row * stride;
		uint16_t *pixels =
			target->pixels + (ptrdiff_t)(y + row) * target->width;

		for (int col = col0; col < col1; ++col) {
			unsigned int alpha = col < box_w ? line[col] : 0;
			uint16_t *pixel = &pixels[x + col];

			if (bold && col > 0 && line[col - 1] > alpha)
				alpha = line[col - 1];
			if (alpha == 0xff)
				*pixel = color;
			else if (alpha)
				*pixel = fplinux_pm_blend(*pixel, color, alpha);
		}
	}
}

/*
 * The glyph of the code point at chars[*index], advancing *index: kerned
 * towards the next code point, '?' when the font lacks it and blank when the
 * font lacks '?' too.
 */
static void next_glyph(const struct fplinux_pm_font *font,
		       const uint16_t *chars, int n, int *index,
		       struct fplinux_pm_glyph *glyph)
{
	uint32_t letter = fplinux_pm_utf16_next(chars, n, index);
	int after = *index;
	uint32_t next = after < n ? fplinux_pm_utf16_next(chars, n, &after) : 0;

	if (font->glyph(font->context, letter, next, glyph) ||
	    font->glyph(font->context, '?', next, glyph))
		return;
	*glyph = (struct fplinux_pm_glyph){ 0 };
}

int fplinux_pm_text_width(const struct fplinux_pm_font *font, bool bold,
			  const uint16_t *chars, int n)
{
	long long width = 0;
	int index = 0;

	while (index < n) {
		struct fplinux_pm_glyph glyph;

		next_glyph(font, chars, n, &index, &glyph);
		width += glyph.advance + (bold ? 1 : 0);
	}
	return width < INT_MAX ? (int)width : INT_MAX;
}

static void draw_glyph(const struct fplinux_pm_font *font,
		       const struct fplinux_pm_glyph *glyph,
		       const struct fplinux_pm_target *target,
		       const struct fplinux_pm_rect *clip, int x, int y,
		       bool bold, uint16_t color)
{
	int stride = 0;
	const uint8_t *coverage = font->coverage(font->context, &stride);

	if (coverage)
		fplinux_pm_glyph_blend(target, clip, x, y, coverage, stride,
				       glyph->box_w, glyph->box_h, bold, color);
	font->release(font->context);
}

/* Fills the rows y1 to y2 - 1 from x1 to x2 - 1, inside clip. */
static void fill(const struct fplinux_pm_target *target,
		 const struct fplinux_pm_rect *clip, long long x1, int y1,
		 long long x2, int y2, uint16_t color)
{
	if (x1 < clip->x1)
		x1 = clip->x1;
	if (x2 > clip->x2)
		x2 = clip->x2;
	if (y1 < clip->y1)
		y1 = clip->y1;
	if (y2 > clip->y2)
		y2 = clip->y2;
	for (int y = y1; y < y2; ++y) {
		uint16_t *row = target->pixels + (ptrdiff_t)y * target->width;

		for (long long x = x1; x < x2; ++x)
			row[x] = color;
	}
}

/* Rows from the baseline to the underline: LVGL's, else one. */
static int underline_offset(const struct fplinux_pm_font *font)
{
	return font->underline_position < 0 ? -font->underline_position : 1;
}

static int underline_thickness(const struct fplinux_pm_font *font)
{
	return font->underline_thickness > 0 ? font->underline_thickness : 1;
}

void fplinux_pm_text_draw(const struct fplinux_pm_font *font,
			  const struct fplinux_pm_font_choice *choice,
			  const struct fplinux_pm_target *target,
			  const struct fplinux_pm_rect *clip, uint16_t color,
			  int x, int y, const uint16_t *chars, int n)
{
	const int line = font->line_height;
	const int bold = choice->synthetic_bold ? 1 : 0;
	const int baseline_offset = line - font->base_line;
	/* 64 bits: a Java coordinate near the int range cannot overflow. */
	long long pen = x;
	int index = 0;

	/* Nothing of a line this far above or below the clip reaches it. */
	if (y >= clip->y2 + line || y <= clip->y1 - 2 * line)
		return;
	/* Glyphs starting a line height right of the clip cannot reach it. */
	while (index < n && pen < clip->x2 + line) {
		struct fplinux_pm_glyph glyph;
		long long left;
		int top;

		next_glyph(font, chars, n, &index, &glyph);
		left = pen + glyph.ofs_x;
		top = y + baseline_offset - glyph.box_h - glyph.ofs_y;
		if (glyph.box_w > 0 && glyph.box_h > 0 && left < clip->x2 &&
		    left + glyph.box_w + bold > clip->x1 && top < clip->y2 &&
		    top + glyph.box_h > clip->y1)
			draw_glyph(font, &glyph, target, clip, (int)left, top,
				   bold, color);
		pen += glyph.advance + bold;
	}
	if (choice->underline) {
		int top = y + baseline_offset + underline_offset(font);

		fill(target, clip, x, top, pen, top + underline_thickness(font),
		     color);
	}
}
