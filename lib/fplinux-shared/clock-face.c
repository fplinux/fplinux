/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-face.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

void fplinux_clock_face_refresh(struct fplinux_clock_face *face,
				lv_obj_t *parent, struct fplinux_ui *ui,
				int radius, int hour, int minute, int second)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(ui);

	if (!face->dial) {
		face->dial = lv_obj_create(parent);
		lv_obj_remove_style_all(face->dial);
		lv_obj_set_scrollable(face->dial, false);
		lv_obj_set_style_radius(face->dial, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(face->dial, metrics->focus_width,
					      0);
		lv_obj_set_style_border_color(face->dial, theme->border, 0);
		for (size_t i = 0; i < 3; ++i) {
			face->hands[i] = lv_line_create(face->dial);
			lv_obj_set_style_line_color(
				face->hands[i],
				i == 2 ? theme->brand_foreground :
					 theme->foreground,
				0);
			lv_obj_set_style_line_width(
				face->hands[i],
				i == 2 ? 1 : metrics->focus_width, 0);
		}
	}
	lv_obj_set_hidden(face->dial, false);
	lv_obj_set_size(face->dial, 2 * radius, 2 * radius);
	for (size_t i = 0; i < 3; ++i) {
		double units = i == 0 ? (hour % 12 + minute / 60.0) / 12.0 :
			       i == 1 ? (minute + second / 60.0) / 60.0 :
					second / 60.0;
		double angle = units * 6.283185307179586;
		int length = radius * (i == 0 ? 5 : 8) / 10;
		int dx = (int)(sin(angle) * length);
		int dy = -(int)(cos(angle) * length);
		int odd = lv_obj_get_style_line_width(face->hands[i], 0) % 2;
		bool steep = abs(dy) >= abs(dx);
		/*
		 * The hands pivot on the dial's centre, the corner between its
		 * four middle pixels. LVGL places a child inside the dial's
		 * border, so that corner is radius - border from the child's
		 * origin. LVGL centres an even-width line on its points and
		 * puts an odd-width one on the pixels after them across its
		 * run; one pixel back across the run puts an odd hand's middle
		 * just before the centre, the odd remainder after it.
		 */
		int centre =
			radius - lv_obj_get_style_border_width(face->dial, 0);
		lv_point_precise_t pivot = {
			centre - (steep ? odd : 0),
			centre - (steep ? 0 : odd),
		};

		face->points[i][0] = pivot;
		face->points[i][1] =
			(lv_point_precise_t){ pivot.x + dx, pivot.y + dy };
		lv_line_set_points(face->hands[i], face->points[i], 2);
	}
}

void fplinux_clock_digits_refresh(struct fplinux_clock_digits *digits,
				  lv_obj_t *label, const char *text,
				  const lv_font_t *font, int width,
				  const struct fplinux_ui_theme *theme)
{
	lv_point_t size;
	int x;
	size_t used = 0;

	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	/* LVGL centres the line and the label independently, rounding each. */
	x = width / 2 - size.x / 2;
	for (size_t i = 0; text[i]; ++i) {
		unsigned char letter = (unsigned char)text[i];
		unsigned char next = (unsigned char)text[i + 1];
		int advance = lv_font_get_glyph_width(font, letter, next);

		if (letter == ':' && used < 2) {
			lv_obj_t *colon = digits->colons[used];

			if (!colon) {
				colon = lv_label_create(label);
				lv_obj_remove_style_all(colon);
				lv_label_set_text(colon, ":");
				lv_obj_set_style_text_align(
					colon, LV_TEXT_ALIGN_LEFT, 0);
				digits->colons[used] = colon;
			}
			lv_obj_set_style_bg_color(colon, theme->background, 0);
			lv_obj_set_style_bg_opa(colon, LV_OPA_COVER, 0);
			lv_obj_set_style_text_color(colon,
						    theme->brand_foreground, 0);
			lv_obj_set_style_text_font(colon, font, 0);
			lv_obj_set_size(colon, advance,
					lv_font_get_line_height(font));
			lv_obj_set_pos(colon, x, 0);
			lv_obj_set_hidden(colon, false);
			++used;
		}
		x += advance;
	}
	while (used < 2) {
		if (digits->colons[used])
			lv_obj_set_hidden(digits->colons[used], true);
		++used;
	}
}
