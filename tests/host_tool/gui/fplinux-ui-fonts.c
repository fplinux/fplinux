/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-fonts.h"

#include <assert.h>
#include <stdio.h>

static unsigned int advance(const lv_font_t *font, uint32_t character,
			    uint32_t next)
{
	lv_font_glyph_dsc_t glyph;

	assert(lv_font_get_glyph_dsc(font, &glyph, character, next));
	return glyph.adv_w;
}

int main(void)
{
	const lv_font_t *font;
	lv_font_glyph_dsc_t glyph;

	lv_init();
	font = fplinux_ui_builtin_font("test-20");
	assert(font);
	assert(!fplinux_ui_builtin_font(NULL));
	assert(!fplinux_ui_builtin_font("test-2"));
	assert(!fplinux_ui_builtin_font("unknown"));
	assert(lv_font_get_glyph_dsc(font, &glyph, 'H', 0));
	assert(font->line_height >= glyph.box_h);
	printf("latin=%u cyrillic=%u kerned=%u cap=%u x=%u\n",
	       advance(font, 'A', 0), advance(font, 0x410, 0),
	       advance(font, 'A', 'V'), font->cap_height, font->x_height);
	for (uint32_t character = '0'; character <= '9'; ++character)
		assert(advance(font, character, 0) == 12);
	lv_deinit();
	return 0;
}
