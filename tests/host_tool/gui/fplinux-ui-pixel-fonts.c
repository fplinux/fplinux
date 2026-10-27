/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-fonts.h"

#include <assert.h>
#include <stdio.h>

static unsigned int advance(const lv_font_t *font, uint32_t character,
			    uint32_t next)
{
	lv_font_glyph_dsc_t glyph = { 0 };

	assert(lv_font_get_glyph_dsc(font, &glyph, character, next));
	assert(!glyph.is_placeholder);
	return glyph.adv_w;
}

static int string_width(const lv_font_t *font, const char *text)
{
	lv_point_t size;

	lv_text_get_size(&size, text, font, 0, 0, INT32_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

static unsigned int partial_alpha(const lv_font_t *font, uint32_t character)
{
	lv_font_glyph_dsc_t glyph = { 0 };
	const unsigned char *bitmap;
	unsigned int partial = 0;
	size_t pixels;

	assert(lv_font_get_glyph_dsc(font, &glyph, character, 0));
	assert(!glyph.is_placeholder);
	assert(glyph.format == LV_FONT_GLYPH_FORMAT_A4);
	assert(!glyph.stride);
	if (!glyph.box_w || !glyph.box_h)
		return 0;
	glyph.req_raw_bitmap = 1;
	bitmap = font->get_glyph_bitmap(&glyph, NULL);
	assert(bitmap);
	pixels = (size_t)glyph.box_w * glyph.box_h;
	for (size_t i = 0; i < pixels; ++i) {
		unsigned int alpha = (bitmap[i / 2] >> (i % 2 ? 0 : 4)) & 15;

		partial += alpha > 0 && alpha < 15;
	}
	lv_font_glyph_release_draw_data(&glyph);
	return partial;
}

static unsigned int own_alpha(const lv_font_t *font, unsigned int *glyphs)
{
	const uint32_t ranges[][2] = {
		{ 0x20, 0x7e },	    { 0xa0, 0xff },	{ 0x400, 0x45f },
		{ 0x2010, 0x2027 }, { 0x2030, 0x2030 }, { 0x2039, 0x203a },
		{ 0x20ac, 0x20ac }, { 0x20bd, 0x20bd }, { 0x2116, 0x2116 },
		{ 0x2122, 0x2122 }, { 0x2190, 0x2193 }, { 0x2212, 0x2212 },
	};
	unsigned int partial = 0;

	for (unsigned int i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
		for (uint32_t code = ranges[i][0]; code <= ranges[i][1];
		     ++code) {
			partial += partial_alpha(font, code);
			++*glyphs;
		}
	}
	return partial;
}

static unsigned int h_stem(const lv_font_t *font)
{
	lv_font_glyph_dsc_t glyph = { 0 };
	const unsigned char *bitmap;
	unsigned int width = 0;

	assert(lv_font_get_glyph_dsc(font, &glyph, 'H', 0));
	glyph.req_raw_bitmap = 1;
	bitmap = font->get_glyph_bitmap(&glyph, NULL);
	assert(bitmap);
	while (width < glyph.box_w) {
		unsigned int alpha =
			(bitmap[width / 2] >> (width % 2 ? 0 : 4)) & 15;

		if (!alpha)
			break;
		++width;
	}
	lv_font_glyph_release_draw_data(&glyph);
	return width;
}

int main(int argc, char **argv)
{
	const lv_font_t *font;
	lv_font_glyph_dsc_t one = { 0 }, h = { 0 }, checkmark = { 0 };
	unsigned int glyphs = 0, partial;

	assert(argc == 2);
	lv_init();
	font = fplinux_ui_builtin_font(argv[1]);
	assert(font);
	partial = own_alpha(font, &glyphs);
	assert(lv_font_get_glyph_dsc(font, &one, '1', 0));
	assert(lv_font_get_glyph_dsc(font, &h, 'H', 0));
	assert(lv_font_get_glyph_dsc(font, &checkmark, 0x2713, 0));
	printf("{\"own_glyphs\":%u,\"own_partial_alpha\":%u,"
	       "\"line\":%d,\"cap\":%d,\"x\":%d,\"digits\":[",
	       glyphs, partial, font->line_height, font->cap_height,
	       font->x_height);
	for (uint32_t code = '0'; code <= '9'; ++code)
		printf("%s%u", code == '0' ? "" : ",", advance(font, code, 0));
	printf("],\"digit_pairs\":[");
	for (uint32_t left = '0'; left <= '9'; ++left) {
		for (uint32_t right = '0'; right <= '9'; ++right)
			printf("%s%u", left == '0' && right == '0' ? "" : ",",
			       advance(font, left, right));
	}
	printf("],\"strings\":[%d,%d,%d],\"one_offset\":%d,\"one_width\":%u,"
	       "\"h_width\":%u,\"h_stem\":%u,\"a_advance\":%u,"
	       "\"av_advance\":%u,\"checkmark_pixels\":%zu,"
	       "\"checkmark_partial_alpha\":%u}\n",
	       string_width(font, "74"), string_width(font, "77"),
	       string_width(font, "0123456789"), one.ofs_x, one.box_w, h.box_w,
	       h_stem(font), advance(font, 'A', 0), advance(font, 'A', 'V'),
	       (size_t)checkmark.box_w * checkmark.box_h,
	       partial_alpha(font, 0x2713));
	lv_deinit();
	return 0;
}
