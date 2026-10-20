/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_TEST_LVGL_H
#define FPLINUX_TERMINAL_TEST_LVGL_H

#include <stdbool.h>
#include <stdint.h>

/* The drawing boundary records geometry; it does not rasterize a font. */
typedef struct {
	int32_t line_height;
} lv_font_t;
typedef struct {
	unsigned int unused;
} lv_layer_t;
typedef uint32_t lv_color_t;
typedef struct {
	int32_t x, y;
} lv_point_t;
typedef struct {
	int32_t x1, y1, x2, y2;
} lv_area_t;
typedef struct {
	unsigned int adv_w, box_w;
	int ofs_x;
	bool is_placeholder;
} lv_font_glyph_dsc_t;
typedef struct {
	lv_color_t bg_color;
	unsigned int bg_opa;
} lv_draw_rect_dsc_t;
typedef struct {
	const lv_font_t *font;
	uint32_t unicode;
	lv_color_t color;
	unsigned int opa;
} lv_draw_letter_dsc_t;

#define LV_OPA_COVER 255

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
			   uint32_t character, uint32_t next);
int32_t lv_font_get_line_height(const lv_font_t *font);
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *glyph);
void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *descriptor);
void lv_draw_letter_dsc_init(lv_draw_letter_dsc_t *descriptor);
void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *descriptor,
		  const lv_area_t *area);
void lv_draw_letter(lv_layer_t *layer, const lv_draw_letter_dsc_t *descriptor,
		    const lv_point_t *position);
lv_color_t lv_color_make(uint8_t red, uint8_t green, uint8_t blue);
lv_color_t lv_color_hex(uint32_t color);

#endif
