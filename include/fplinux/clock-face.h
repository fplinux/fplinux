/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_FACE_H
#define FPLINUX_CLOCK_FACE_H

#include <fplinux/fplinux-ui.h>

struct fplinux_clock_face {
	lv_obj_t *dial;
	lv_obj_t *hands[3];
	lv_point_precise_t points[3][2];
};

struct fplinux_clock_digits {
	lv_obj_t *colons[2];
};

/* The parent owns the objects; points remain alive with the face. */
void fplinux_clock_face_refresh(struct fplinux_clock_face *face,
				lv_obj_t *parent, struct fplinux_ui *ui,
				int radius, int hour, int minute, int second);

/* The label retains its time string and owns the colour overlays. */
void fplinux_clock_digits_refresh(struct fplinux_clock_digits *digits,
				  lv_obj_t *label, const char *text,
				  const lv_font_t *font, int width,
				  const struct fplinux_ui_theme *theme);

#endif
