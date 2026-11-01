/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-status-font.h"
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-profile.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static uint32_t codepoint(unsigned int index)
{
	return index < 95 ? 0x20 + index : 0x2190 + index - 95;
}

static int adjustment(const struct fplinux_status_font *font, unsigned int left,
		      unsigned int right)
{
	for (unsigned int i = 0; i < font->kerning_count; ++i)
		if (font->kerning[i].left == left &&
		    font->kerning[i].right == right)
			return font->kerning[i].adjustment;
	return 0;
}

static void compare_font(const char *id)
{
	const struct fplinux_status_font *status =
		fplinux_status_font_by_id(id);
	const lv_font_t *ui = fplinux_ui_builtin_font(id);

	assert(status && ui);
	assert(status->line_height == ui->line_height);
	assert(status->base_line == ui->base_line);
	for (unsigned int i = 0; i < 99; ++i) {
		const struct fplinux_status_glyph *actual = &status->glyphs[i];
		lv_font_glyph_dsc_t expected = { 0 };
		const uint8_t *bitmap;

		assert(lv_font_get_glyph_dsc(ui, &expected, codepoint(i), 0));
		assert(!expected.is_placeholder);
		assert(actual->box_w == expected.box_w &&
		       actual->box_h == expected.box_h);
		assert(actual->ofs_x == expected.ofs_x &&
		       actual->ofs_y == expected.ofs_y);
		assert((unsigned int)(actual->advance + 8) / 16 ==
		       expected.adv_w);
		if (actual->box_w && actual->box_h) {
			expected.req_raw_bitmap = 1;
			bitmap = ui->get_glyph_bitmap(&expected, NULL);
			assert(bitmap);
			for (unsigned int pixel = 0;
			     pixel <
			     (unsigned int)actual->box_w * actual->box_h;
			     ++pixel) {
				unsigned int shift = pixel % 2 ? 0 : 4;
				unsigned int got =
					status->bitmap[actual->bitmap_offset +
						       pixel / 2];

				assert(((got >> shift) & 15) ==
				       ((bitmap[pixel / 2] >> shift) & 15));
			}
			lv_font_glyph_release_draw_data(&expected);
		}
		for (unsigned int j = 0; j < 99; ++j) {
			int advance =
				actual->advance + adjustment(status, i, j);

			if (advance < 0)
				advance = 0;
			assert(lv_font_get_glyph_dsc(
				ui, &expected, codepoint(i), codepoint(j)));
			assert((unsigned int)(advance + 8) / 16 ==
			       expected.adv_w);
		}
	}
}

int main(int argc, char **argv)
{
	int first = argc == 2 ? atoi(argv[1]) : 0;
	int end = argc == 2 ? first + 1 : FPLINUX_UI_PROFILE_COUNT;

	assert(argc == 1 || argc == 2);
	lv_init();
	assert(!fplinux_status_font_by_id(NULL));
	assert(!fplinux_status_font_by_id("unknown"));
	if (argc == 2 && first == 0)
		assert(!fplinux_status_font_by_id("noto-sans-13"));
	for (int i = first; i < end; ++i) {
		const struct fplinux_ui_profile_spec *profile =
			fplinux_ui_profile_spec(i);

		assert(profile);
		compare_font(profile->font_body);
		compare_font(profile->font_title);
		compare_font(profile->font_legend);
	}
	lv_deinit();
	return 0;
}
