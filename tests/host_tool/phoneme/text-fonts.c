/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-profile.h"
#include "fplinux-pm-glyph.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct native_font {
	const lv_font_t *font;
	lv_font_glyph_dsc_t glyph;
	lv_draw_buf_t *buffer;
};

static bool describe(void *context, uint32_t letter, uint32_t next,
		     struct fplinux_pm_glyph *out)
{
	struct native_font *native = context;

	if (!lv_font_get_glyph_dsc(native->font, &native->glyph, letter, next))
		return false;
	assert(!native->glyph.is_placeholder);
	*out = (struct fplinux_pm_glyph){
		.advance = native->glyph.adv_w,
		.box_w = native->glyph.box_w,
		.box_h = native->glyph.box_h,
		.ofs_x = native->glyph.ofs_x,
		.ofs_y = native->glyph.ofs_y,
	};
	return true;
}

static const uint8_t *coverage(void *context, int *stride)
{
	struct native_font *native = context;

	native->buffer = lv_draw_buf_create(native->glyph.box_w,
					    native->glyph.box_h,
					    LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
	assert(native->buffer);
	assert(lv_font_get_glyph_bitmap(&native->glyph, native->buffer) ==
	       native->buffer);
	*stride = (int)native->buffer->header.stride;
	return native->buffer->data;
}

static void release(void *context)
{
	struct native_font *native = context;

	lv_font_glyph_release_draw_data(&native->glyph);
	lv_draw_buf_destroy(native->buffer);
	native->buffer = NULL;
}

static void draw(const struct fplinux_pm_font *font,
		 const struct fplinux_pm_font_choice *choice,
		 const uint16_t *text, int length, uint16_t pixels[32][128])
{
	const struct fplinux_pm_target target = {
		.pixels = &pixels[0][0],
		.width = 128,
		.height = 32,
	};
	const struct fplinux_pm_rect clip = { 0, 0, 128, 32 };
	unsigned int ink = 0;

	memset(pixels, 0, 32 * 128 * sizeof(uint16_t));
	fplinux_pm_text_draw(font, choice, &target, &clip, 0xffff, 0, 0, text,
			     length);
	for (int y = 0; y < 32; ++y) {
		for (int x = 0; x < 128; ++x) {
			assert(pixels[y][x] == 0 || pixels[y][x] == 0xffff);
			if (!pixels[y][x])
				continue;
			assert(y < font->line_height);
			++ink;
		}
	}
	assert(ink > 0);
}

static void check_string(const struct fplinux_pm_font *font,
			 const struct fplinux_pm_font_choice *choice,
			 const lv_font_t *native, const uint16_t text[2],
			 const char *utf8)
{
	const uint16_t missing[] = { '?', '?' };
	uint16_t actual[32][128], replacement[32][128];
	lv_point_t size;
	int width;

	lv_text_get_size(&size, utf8, native, 0, 0, INT32_MAX,
			 LV_TEXT_FLAG_NONE);
	width = fplinux_pm_text_width(font, choice->synthetic_bold, text, 2);
	assert(width == size.x + (choice->synthetic_bold ? 2 : 0));
	draw(font, choice, text, 2, actual);
	draw(font, choice, missing, 2, replacement);
	assert(memcmp(actual, replacement, sizeof(actual)));
	if (choice->underline) {
		int y = font->line_height - font->base_line +
			(font->underline_position < 0 ?
				 -font->underline_position :
				 1);

		for (int x = 0; x < width; ++x)
			assert(actual[y][x] == 0xffff);
		assert(actual[y][width] == 0);
	}
}

static void check_slot(int size, enum fplinux_pm_font_slot slot, const char *id,
		       int line_height, int digit_advance)
{
	const uint16_t english[] = { 'A', 'V' };
	const uint16_t russian[] = { 0x401, 0x436 };
	const uint16_t digits[] = { '0', '1', '2', '3', '4',
				    '5', '6', '7', '8', '9' };
	struct native_font native = { .font = fplinux_ui_builtin_font(id) };
	struct fplinux_pm_font font;

	assert(native.font);
	assert(native.font->line_height == line_height);
	font = (struct fplinux_pm_font){
		.line_height = native.font->line_height,
		.base_line = native.font->base_line,
		.underline_position = native.font->underline_position,
		.underline_thickness = native.font->underline_thickness,
		.glyph = describe,
		.coverage = coverage,
		.release = release,
		.context = &native,
	};
	for (int style = 0; style <= 5; ++style) {
		struct fplinux_pm_font_choice choice;
		bool bold = (style & 1) && slot != FPLINUX_PM_FONT_LARGE;

		if (style == 2 || style == 3)
			continue;
		choice = fplinux_pm_font_choose(style, size);
		assert(choice.slot == slot);
		assert(choice.synthetic_bold == bold);
		assert(choice.underline == ((style & 4) != 0));
		assert(fplinux_pm_text_width(&font, bold, digits, 10) ==
		       10 * (digit_advance + (bold ? 1 : 0)));
		check_string(&font, &choice, native.font, english, "AV");
		check_string(&font, &choice, native.font, russian, "Ёж");
	}
}

int main(int argc, char **argv)
{
	const struct fplinux_ui_profile_spec *profile;
	int height;

	assert(argc == 2);
	height = atoi(argv[1]);
	assert(height == 128 || height == 160);
	lv_init();
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(128, height));
	assert(profile);
	check_slot(FPLINUX_PM_SIZE_MEDIUM, FPLINUX_PM_FONT_BODY,
		   profile->font_body, 14, 7);
	check_slot(FPLINUX_PM_SIZE_SMALL, FPLINUX_PM_FONT_SMALL,
		   profile->font_legend, 14, 7);
	check_slot(FPLINUX_PM_SIZE_LARGE, FPLINUX_PM_FONT_LARGE,
		   profile->font_title, 17, 9);
	printf("128x%d: phoneME text preserves EN/RU metrics and pixel glyphs\n",
	       height);
	lv_deinit();
	return 0;
}
