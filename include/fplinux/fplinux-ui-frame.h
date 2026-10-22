/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Owned RGB565 software frames; use from the UI thread only. */
#ifndef FPLINUX_UI_FRAME_H
#define FPLINUX_UI_FRAME_H

#include "fplinux-ui.h"

struct fplinux_ui_frame;

enum fplinux_ui_frame_mode {
	/* One buffer: finish drawing without yielding or refreshing LVGL. */
	FPLINUX_UI_FRAME_DIRECT,
	/* Two buffers: LVGL sees only pixels committed by present(). */
	FPLINUX_UI_FRAME_STAGED,
};

/*
 * Owns a zeroed packed RGB565 buffer and a canvas on the raw UI screen.
 * ui must be non-NULL; dimensions 1..4096, valid mode: otherwise EINVAL.
 * All required allocations are checked: NULL/ENOMEM on failure.
 */
struct fplinux_ui_frame *fplinux_ui_frame_new(struct fplinux_ui *ui, int width,
					      int height,
					      enum fplinux_ui_frame_mode mode);
/* NULL is harmless. Detaches, but does not delete, the borrowed timer. */
void fplinux_ui_frame_delete(struct fplinux_ui_frame *frame);
/*
 * Borrowed canvas: may position, hide or reparent it within its UI tree,
 * never delete it. Parent deletion or UI close detaches it and pauses its
 * timer; explicitly delete the handle afterwards.
 * NULL/EINVAL; deleted canvas: NULL/ENODEV.
 */
lv_obj_t *fplinux_ui_frame_object(struct fplinux_ui_frame *frame);
/*
 * Borrowed drawing pixels; optional width/height outputs, stride = width.
 * Reacquire after resize. NULL frame: NULL/EINVAL.
 */
uint16_t *fplinux_ui_frame_pixels(struct fplinux_ui_frame *frame, int *width,
				  int *height);
/*
 * Explicit geometry, e.g. from FPLINUX_UI_EVENT_RESIZE/content size.
 * Reuses capacity; growth is transactional (false/ENOMEM leaves it intact).
 * A changed geometry clears old damage and the STAGED published image;
 * caller must draw and damage the new frame. Same size is a no-op.
 * false/EINVAL for NULL or invalid size, false/ENODEV if canvas was deleted.
 */
bool fplinux_ui_frame_resize(struct fplinux_ui_frame *frame, int width,
			     int height);
/*
 * Canvas-local inclusive coordinates, clipped and accumulated as a bounding
 * rectangle. NULL area damages the whole frame; empty/outside is a no-op.
 * NULL frame: false/EINVAL; deleted canvas: false/ENODEV.
 */
bool fplinux_ui_frame_damage(struct fplinux_ui_frame *frame,
			     const lv_area_t *area);
/*
 * Commit a completed frame and refresh now. STAGED copies only the damage
 * rows/columns; DIRECT copies nothing. Never call during unfinished paint.
 * Visible UI with no damage: true. Hidden UI: false/EAGAIN, damage retained.
 * NULL: false/EINVAL; deleted canvas: false/ENODEV.
 * Canvas hiding alone does not pause its timer.
 */
bool fplinux_ui_frame_present(struct fplinux_ui_frame *frame);
/*
 * Borrow one application-owned timer; run iff enabled and UI visible.
 * Hide/show automatically pause/resume it, including a hidden start.
 * NULL timer with enabled=false detaches; detach before deleting the timer.
 * Replacing/detaching pauses the old timer. NULL frame or NULL/true: EINVAL.
 * Deleted canvas: ENODEV. No timer is allocated or deleted by this helper.
 */
bool fplinux_ui_frame_set_timer(struct fplinux_ui_frame *frame,
				lv_timer_t *timer, bool enabled);

#endif
