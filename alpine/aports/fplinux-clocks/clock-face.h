/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_FACE_H
#define FPLINUX_CLOCK_FACE_H

#include <fplinux/fplinux-ui.h>

struct fplinux_clock_face {
	lv_obj_t *dial;
	lv_obj_t *hands[3];
	lv_point_precise_t points[3][2];
};

/* The parent owns the objects; points remain alive with the face. */
void fplinux_clock_face_refresh(struct fplinux_clock_face *face,
				lv_obj_t *parent, struct fplinux_ui *ui,
				int radius, int hour, int minute, int second);

#endif
