/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MUSIC_VIEW_H
#define FPLINUX_MUSIC_VIEW_H

#include "fplinux-ui-screen.h"

struct music_view {
	struct fplinux_ui_screen *screen;
	lv_obj_t *title;
	lv_obj_t *artist;
	lv_obj_t *state;
	lv_obj_t *time;
	lv_obj_t *progress;
	lv_obj_t *hint;
};

bool music_view_create(struct music_view *view, struct fplinux_ui *ui);
void music_view_layout(struct music_view *view, struct fplinux_ui *ui);
void music_view_update_volume(struct music_view *view, struct fplinux_ui *ui,
			      bool warning);
void music_view_update(struct music_view *view, const char *title,
		       const char *artist, const char *state,
		       uint64_t position_ms, uint64_t duration_ms,
		       bool duration_known, bool empty);

#endif
