/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-face.h"

#include <math.h>

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
				i == 2 ? theme->accent : theme->foreground, 0);
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

		face->points[i][0] = (lv_point_precise_t){ radius, radius };
		face->points[i][1] = (lv_point_precise_t){
			radius + (int)(sin(angle) * length),
			radius - (int)(cos(angle) * length)
		};
		lv_line_set_points(face->hands[i], face->points[i], 2);
	}
}
