/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-frame.h"
#include "fplinux-ui-frame-private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_frame {
	struct fplinux_ui_frame *next;
	struct fplinux_ui *ui;
	lv_obj_t *canvas;
	uint16_t *pixels;
	uint16_t *published;
	size_t capacity;
	int width;
	int height;
	lv_area_t damage;
	bool damaged;
	lv_timer_t *timer;
	bool enabled;
};

/* All access is on the process's UI thread, like LVGL itself. */
static struct fplinux_ui_frame *frames;

static bool valid_size(int width, int height)
{
	if (width > 0 && width <= 4096 && height > 0 && height <= 4096)
		return true;
	errno = EINVAL;
	return false;
}

static bool live(struct fplinux_ui_frame *frame)
{
	if (frame && frame->canvas)
		return true;
	errno = frame ? ENODEV : EINVAL;
	return false;
}

static void run_timer(struct fplinux_ui_frame *frame, bool visible)
{
	if (!frame->timer)
		return;
	if (visible && frame->enabled)
		lv_timer_resume(frame->timer);
	else
		lv_timer_pause(frame->timer);
}

void fplinux_ui_frames_set_visible(struct fplinux_ui *ui, bool visible)
{
	for (struct fplinux_ui_frame *frame = frames; frame;
	     frame = frame->next)
		if (frame->ui == ui)
			run_timer(frame, visible);
}

static void canvas_deleted(lv_event_t *event)
{
	struct fplinux_ui_frame *frame = lv_event_get_user_data(event);

	run_timer(frame, false);
	frame->timer = NULL;
	frame->canvas = NULL;
	frame->ui = NULL;
}

static void frame_bind(struct fplinux_ui_frame *frame)
{
	lv_canvas_set_buffer(
		frame->canvas,
		frame->published ? frame->published : frame->pixels,
		frame->width, frame->height, LV_COLOR_FORMAT_RGB565);
}

struct fplinux_ui_frame *fplinux_ui_frame_new(struct fplinux_ui *ui, int width,
					      int height,
					      enum fplinux_ui_frame_mode mode)
{
	struct fplinux_ui_frame *frame;
	size_t count;

	if (!ui || (mode != FPLINUX_UI_FRAME_DIRECT &&
		    mode != FPLINUX_UI_FRAME_STAGED)) {
		errno = EINVAL;
		return NULL;
	}
	if (!valid_size(width, height))
		return NULL;
	frame = calloc(1, sizeof(*frame));
	if (!frame)
		goto no_memory;
	count = (size_t)width * (size_t)height;
	frame->pixels = calloc(count, sizeof(*frame->pixels));
	if (!frame->pixels)
		goto no_memory;
	if (mode == FPLINUX_UI_FRAME_STAGED) {
		frame->published = calloc(count, sizeof(*frame->published));
		if (!frame->published)
			goto no_memory;
	}
	frame->canvas = lv_canvas_create(fplinux_ui_screen(ui));
	if (!frame->canvas)
		goto no_memory;
	/* Registration is a required resource as well as canvas allocation. */
	if (!lv_obj_add_event_cb(frame->canvas, canvas_deleted, LV_EVENT_DELETE,
				 frame)) {
		lv_obj_delete(frame->canvas);
		frame->canvas = NULL;
		goto no_memory;
	}
	frame->ui = ui;
	frame->width = width;
	frame->height = height;
	frame->capacity = count;
	lv_obj_set_pos(frame->canvas, 0, 0);
	lv_obj_set_scrollable(frame->canvas, false);
	lv_obj_set_clickable(frame->canvas, false);
	frame_bind(frame);
	frame->next = frames;
	frames = frame;
	return frame;

no_memory:
	if (frame) {
		free(frame->published);
		free(frame->pixels);
		free(frame);
	}
	errno = ENOMEM;
	return NULL;
}

void fplinux_ui_frame_delete(struct fplinux_ui_frame *frame)
{
	struct fplinux_ui_frame **link = &frames;

	if (!frame)
		return;
	while (*link && *link != frame)
		link = &(*link)->next;
	if (*link)
		*link = frame->next;
	if (frame->canvas)
		lv_obj_delete(frame->canvas);
	free(frame->published);
	free(frame->pixels);
	free(frame);
}

lv_obj_t *fplinux_ui_frame_object(struct fplinux_ui_frame *frame)
{
	return live(frame) ? frame->canvas : NULL;
}

uint16_t *fplinux_ui_frame_pixels(struct fplinux_ui_frame *frame, int *width,
				  int *height)
{
	if (!frame) {
		errno = EINVAL;
		return NULL;
	}
	if (width)
		*width = frame->width;
	if (height)
		*height = frame->height;
	return frame->pixels;
}

bool fplinux_ui_frame_resize(struct fplinux_ui_frame *frame, int width,
			     int height)
{
	size_t count;
	uint16_t *pixels;
	uint16_t *published = NULL;

	if (!live(frame) || !valid_size(width, height))
		return false;
	if (frame->width == width && frame->height == height)
		return true;
	count = (size_t)width * (size_t)height;
	if (count > frame->capacity) {
		pixels = calloc(count, sizeof(*pixels));
		if (!pixels)
			goto no_memory;
		if (frame->published) {
			published = calloc(count, sizeof(*published));
			if (!published) {
				free(pixels);
				goto no_memory;
			}
		}
		free(frame->pixels);
		free(frame->published);
		frame->pixels = pixels;
		frame->published = published;
		frame->capacity = count;
	} else if (frame->published) {
		/* Never reinterpret a completed frame with a different stride. */
		memset(frame->published, 0, count * sizeof(*frame->published));
	}
	frame->width = width;
	frame->height = height;
	frame->damaged = false;
	frame_bind(frame);
	return true;

no_memory:
	errno = ENOMEM;
	return false;
}

bool fplinux_ui_frame_damage(struct fplinux_ui_frame *frame,
			     const lv_area_t *area)
{
	lv_area_t clipped;

	if (!live(frame))
		return false;
	clipped =
		area ? *area :
		       (lv_area_t){ 0, 0, frame->width - 1, frame->height - 1 };
	if (clipped.x1 < 0)
		clipped.x1 = 0;
	if (clipped.y1 < 0)
		clipped.y1 = 0;
	if (clipped.x2 >= frame->width)
		clipped.x2 = frame->width - 1;
	if (clipped.y2 >= frame->height)
		clipped.y2 = frame->height - 1;
	if (clipped.x1 > clipped.x2 || clipped.y1 > clipped.y2)
		return true;
	if (!frame->damaged) {
		frame->damage = clipped;
		frame->damaged = true;
		return true;
	}
	if (clipped.x1 < frame->damage.x1)
		frame->damage.x1 = clipped.x1;
	if (clipped.y1 < frame->damage.y1)
		frame->damage.y1 = clipped.y1;
	if (clipped.x2 > frame->damage.x2)
		frame->damage.x2 = clipped.x2;
	if (clipped.y2 > frame->damage.y2)
		frame->damage.y2 = clipped.y2;
	return true;
}

bool fplinux_ui_frame_present(struct fplinux_ui_frame *frame)
{
	lv_area_t coords;
	lv_area_t area;

	if (!live(frame))
		return false;
	if (!fplinux_ui_is_visible(frame->ui)) {
		errno = EAGAIN;
		return false;
	}
	if (!frame->damaged)
		return true;
	if (frame->published) {
		for (int y = frame->damage.y1; y <= frame->damage.y2; ++y) {
			size_t offset = (size_t)y * (size_t)frame->width +
					(size_t)frame->damage.x1;
			size_t count = (size_t)(frame->damage.x2 -
						frame->damage.x1 + 1);

			memcpy(frame->published + offset,
			       frame->pixels + offset,
			       count * sizeof(*frame->pixels));
		}
	}
	lv_obj_update_layout(frame->canvas);
	lv_obj_get_coords(frame->canvas, &coords);
	area = frame->damage;
	lv_area_move(&area, coords.x1, coords.y1);
	lv_obj_invalidate_area(frame->canvas, &area);
	frame->damaged = false;
	lv_refr_now(NULL);
	return true;
}

bool fplinux_ui_frame_set_timer(struct fplinux_ui_frame *frame,
				lv_timer_t *timer, bool enabled)
{
	if (!live(frame))
		return false;
	if (!timer && enabled) {
		errno = EINVAL;
		return false;
	}
	if (frame->timer && frame->timer != timer)
		lv_timer_pause(frame->timer);
	frame->timer = timer;
	frame->enabled = enabled;
	run_timer(frame, fplinux_ui_is_visible(frame->ui));
	return true;
}
