/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_TEST_LVGL_H
#define FPLINUX_TERMINAL_TEST_LVGL_H

#include <stdbool.h>
#include <stdint.h>

/* Record drawing calls while exercising the real PSF adapter callbacks. */
typedef struct lv_font lv_font_t;
typedef struct lv_font_glyph_dsc lv_font_glyph_dsc_t;
typedef struct {
	struct {
		unsigned int stride;
	} header;
	unsigned char *data;
} lv_draw_buf_t;
struct lv_font {
	int32_t line_height;
	const void *dsc;
	bool (*get_glyph_dsc)(const lv_font_t *font,
			      lv_font_glyph_dsc_t *descriptor,
			      uint32_t character, uint32_t next);
	const void *(*get_glyph_bitmap)(lv_font_glyph_dsc_t *descriptor,
					lv_draw_buf_t *draw_buffer);
	unsigned int kerning, static_bitmap;
};
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
struct lv_font_glyph_dsc {
	unsigned int adv_w, box_w, box_h, stride, format;
	int ofs_x, ofs_y;
	bool is_placeholder;
	bool req_raw_bitmap;
	const lv_font_t *resolved_font;
	union {
		uint32_t index;
	} gid;
};
typedef struct {
	lv_color_t bg_color;
	unsigned int bg_opa;
} lv_draw_rect_dsc_t;
typedef struct {
	const lv_font_t *font;
	lv_color_t color;
	unsigned int opa;
} lv_draw_label_dsc_t;

#define LV_OPA_COVER 255
#define LV_FONT_GLYPH_FORMAT_A1 1
#define LV_FONT_KERNING_NONE 0

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
			   uint32_t character, uint32_t next);
int32_t lv_font_get_line_height(const lv_font_t *font);
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *glyph);
void lv_draw_buf_flush_cache(lv_draw_buf_t *draw_buffer, const lv_area_t *area);
void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *descriptor);
void lv_draw_label_dsc_init(lv_draw_label_dsc_t *descriptor);
void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *descriptor,
		  const lv_area_t *area);
void lv_draw_character(lv_layer_t *layer, lv_draw_label_dsc_t *descriptor,
		       const lv_point_t *position, uint32_t character);
lv_color_t lv_color_make(uint8_t red, uint8_t green, uint8_t blue);
lv_color_t lv_color_hex(uint32_t color);

#endif
