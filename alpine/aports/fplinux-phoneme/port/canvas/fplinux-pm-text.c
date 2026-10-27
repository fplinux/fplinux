// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme Canvas text: the putpixel port text interface of
 * putpixel_port/include/gxjport_text.h in the runtime's theme fonts (LVGL
 * bitmap fonts with Cyrillic). gx_putpixel's gxj_text.c calls it
 * for Graphics.drawString() and Font metrics; when it returns KNI_FALSE or
 * -1, as without a runtime connection (the headless runner), gxj_text.c uses
 * its built-in bitmap font. The natives run on the runtime thread inside a VM
 * slice, so they may use LVGL directly.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fplinux/fplinux-ui.h>

#include <kni.h>
#include <gxapi_constants.h>
#include <font_constants.h>
#include <gxj_putpixel.h>
#include <gxjport_text.h>

#include "../app/fplinux-pm-app.h"
#include "fplinux-pm-glyph.h"

_Static_assert(FPLINUX_PM_STYLE_BOLD == STYLE_BOLD &&
		       FPLINUX_PM_STYLE_UNDERLINED == STYLE_UNDERLINED &&
		       FPLINUX_PM_SIZE_MEDIUM == SIZE_MEDIUM &&
		       FPLINUX_PM_SIZE_SMALL == SIZE_SMALL &&
		       FPLINUX_PM_SIZE_LARGE == SIZE_LARGE,
	       "fplinux-pm-glyph.h has the Font constants of gxapi_constants.h");

/* A theme font and the glyph its callbacks looked up last. */
struct theme_font {
	const lv_font_t *font;
	lv_font_glyph_dsc_t glyph;
};

/* The A8 buffer glyphs are decoded into, grown to the largest glyph. */
static lv_draw_buf_t *scratch;

/*
 * The theme font of a MIDP font; NULL without a runtime connection. The
 * size reaching the port is Font.init()'s OEM_FONT_SIZE() of a SIZE_*
 * constant (font_constants.h), or a pixel size for Font.getOEMFont(), so
 * LCDUI_FONT_SIZE() turns it back into the constant.
 */
static const lv_font_t *choose_font(int style, int size,
				    struct fplinux_pm_font_choice *choice)
{
	struct fplinux_ui *ui = fplinux_pm_app_ui();
	const struct fplinux_ui_theme *theme = ui ? fplinux_ui_theme(ui) : NULL;

	if (!theme)
		return NULL;
	*choice = fplinux_pm_font_choose(style, LCDUI_FONT_SIZE(size));
	switch (choice->slot) {
	case FPLINUX_PM_FONT_LARGE:
		return theme->font_large;
	case FPLINUX_PM_FONT_SMALL:
		return theme->font_softkey;
	default:
		return theme->font;
	}
}

static bool font_glyph(void *context, uint32_t letter, uint32_t next,
		       struct fplinux_pm_glyph *out)
{
	struct theme_font *font = context;
	const lv_font_glyph_dsc_t *glyph = &font->glyph;
	bool bitmap;

	if (!lv_font_get_glyph_dsc(font->font, &font->glyph, letter, next))
		return false;
	/* The text fonts have only bitmaps; anything else stays blank. */
	bitmap = glyph->format > LV_FONT_GLYPH_FORMAT_NONE &&
		 glyph->format < LV_FONT_GLYPH_FORMAT_IMAGE;
	*out = (struct fplinux_pm_glyph){
		.advance = glyph->adv_w,
		.box_w = bitmap ? glyph->box_w : 0,
		.box_h = bitmap ? glyph->box_h : 0,
		.ofs_x = glyph->ofs_x,
		.ofs_y = glyph->ofs_y,
	};
	return true;
}

/* The scratch buffer shaped for a box_w x box_h glyph, grown if needed. */
static lv_draw_buf_t *glyph_buffer(uint32_t box_w, uint32_t box_h)
{
	lv_draw_buf_t *buffer = lv_draw_buf_reshape(
		scratch, LV_COLOR_FORMAT_A8, box_w, box_h, LV_STRIDE_AUTO);

	if (buffer)
		return buffer;
	lv_draw_buf_destroy(scratch);
	/* Room for taller glyphs too, as lv_draw_label does. */
	scratch = lv_draw_buf_create(box_w, LV_ROUND_UP(box_h, 32),
				     LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
	return lv_draw_buf_reshape(scratch, LV_COLOR_FORMAT_A8, box_w, box_h,
				   LV_STRIDE_AUTO);
}

static const uint8_t *font_coverage(void *context, int *stride)
{
	struct theme_font *font = context;
	lv_font_glyph_dsc_t *glyph = &font->glyph;
	lv_draw_buf_t *buffer = glyph_buffer(glyph->box_w, glyph->box_h);

	/* Bitmap fonts decode into the buffer and return it. */
	if (!buffer || lv_font_get_glyph_bitmap(glyph, buffer) != buffer)
		return NULL;
	*stride = (int)buffer->header.stride;
	return buffer->data;
}

static void font_release(void *context)
{
	struct theme_font *font = context;

	lv_font_glyph_release_draw_data(&font->glyph);
}

static struct fplinux_pm_font glyph_font(struct theme_font *font)
{
	return (struct fplinux_pm_font){
		.line_height = font->font->line_height,
		.base_line = font->font->base_line,
		.underline_position = font->font->underline_position,
		.underline_thickness = font->font->underline_thickness,
		.glyph = font_glyph,
		.coverage = font_coverage,
		.release = font_release,
		.context = font,
	};
}

/*
 * x, y is the top left of the text, gxj_text.c having applied the anchor
 * with gxjport_get_chars_width() and gxjport_get_font_info(). Like the
 * putpixel primitives, text changes only pixelData: a mutable image has no
 * alpha. Dotted strokes and the Font face do not apply.
 */
int gxjport_draw_chars(int pixel, const jshort *clip, void *dst, int dotted,
		       int face, int style, int size, int x, int y, int anchor,
		       const jchar *chararray, int n)
{
	const gxj_screen_buffer *buffer = dst;
	struct fplinux_pm_font_choice choice;
	struct theme_font font = { .font = choose_font(style, size, &choice) };
	struct fplinux_pm_target target;
	struct fplinux_pm_rect area;
	struct fplinux_pm_font metrics;

	(void)dotted;
	(void)face;
	(void)anchor;
	if (!font.font)
		return KNI_FALSE;
	if (n <= 0 || !buffer || !buffer->pixelData ||
	    !fplinux_pm_clip(clip, buffer->width, buffer->height, &area))
		return KNI_TRUE;
	target = (struct fplinux_pm_target){
		.pixels = buffer->pixelData,
		.width = buffer->width,
		.height = buffer->height,
	};
	metrics = glyph_font(&font);
	fplinux_pm_text_draw(&metrics, &choice, &target, &area,
			     (uint16_t)GXJ_RGB24TORGB16(pixel), x, y, chararray,
			     n);
	return KNI_TRUE;
}

/*
 * The LVGL line: ascent above the baseline, base_line below it and no
 * leading, so Font.getHeight() is the line height and the BASELINE anchor
 * puts LVGL's baseline at the anchor's y.
 */
int gxjport_get_font_info(int face, int style, int size, int *ascent,
			  int *descent, int *leading)
{
	struct fplinux_pm_font_choice choice;
	const lv_font_t *font = choose_font(style, size, &choice);

	(void)face;
	if (!font)
		return KNI_FALSE;
	*ascent = font->line_height - font->base_line;
	*descent = font->base_line;
	*leading = 0;
	return KNI_TRUE;
}

int gxjport_get_chars_width(int face, int style, int size,
			    const jchar *charArray, int n)
{
	struct fplinux_pm_font_choice choice;
	struct theme_font font = { .font = choose_font(style, size, &choice) };
	struct fplinux_pm_font metrics;

	(void)face;
	if (!font.font)
		return -1;
	if (n <= 0)
		return 0;
	metrics = glyph_font(&font);
	return fplinux_pm_text_width(&metrics, choice.synthetic_bold, charArray,
				     n);
}
