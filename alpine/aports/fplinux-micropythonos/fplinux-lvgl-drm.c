/* SPDX-License-Identifier: GPL-2.0-only */
/* fplinux-check: package-embedded */
#define _GNU_SOURCE

#include "lv_linux_drm.h"
#include "../../../../lvgl.h"
#include "fplinux-drm-session.h"
#include "fplinux-keypad-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fplinux_lvgl_display {
	struct fplinux_drm_session session;
	lv_timer_t *timer;
	bool opened;
};

static uint32_t tick_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000U +
			  now.tv_nsec / 1000000U);
}

static void display_error(const char *operation)
{
	fprintf(stderr, "MicroPythonOS display: %s: %s\n", operation,
		strerror(errno));
	exit(EXIT_FAILURE);
}

static bool set_active(struct fplinux_drm_session *session, bool active,
		       void *data)
{
	lv_display_t *display = data;

	(void)session;
	if (!fplinux_keypad_set_active(active))
		return false;
	if (active)
		lv_obj_invalidate(lv_display_get_screen_active(display));
	return true;
}

static void dispatch_vt(lv_timer_t *timer)
{
	struct fplinux_lvgl_display *driver = lv_timer_get_user_data(timer);

	if (!fplinux_drm_session_dispatch(&driver->session))
		display_error("VT dispatch");
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	struct fplinux_lvgl_display *driver =
		lv_display_get_driver_data(display);
	struct fplinux_drm_session *session = &driver->session;
	unsigned int page;

	(void)area;
	if (!fplinux_drm_session_dispatch(session))
		display_error("VT dispatch");
	if (session->active && lv_display_flush_is_last(display)) {
		page = pixels == session->mapping ? 0U : 1U;
		if (pixels != session->mapping + page * session->page_bytes) {
			errno = EINVAL;
			display_error("unexpected render buffer");
		}
		if (!fplinux_drm_session_present(session, page))
			display_error("atomic commit");
	}
	lv_display_flush_ready(display);
}

static void delete_display(lv_event_t *event)
{
	lv_display_t *display = lv_event_get_target(event);
	struct fplinux_lvgl_display *driver =
		lv_display_get_driver_data(display);

	if (driver->timer)
		lv_timer_delete(driver->timer);
	if (driver->opened)
		fplinux_drm_session_close(&driver->session);
	lv_free(driver);
}

lv_display_t *lv_linux_drm_create(void)
{
	lv_display_t *display = lv_display_create(1, 1);
	struct fplinux_lvgl_display *driver;

	lv_tick_set_cb(tick_ms);
	if (!display)
		return NULL;
	driver = lv_malloc_zeroed(sizeof(*driver));
	if (!driver) {
		lv_display_delete(display);
		return NULL;
	}
	lv_display_set_driver_data(display, driver);
	lv_display_set_flush_cb(display, flush);
	lv_display_add_event_cb(display, delete_display, LV_EVENT_DELETE, NULL);
	return display;
}

void lv_linux_drm_set_file(lv_display_t *display, const char *file,
			   int64_t connector_id)
{
	struct fplinux_lvgl_display *driver =
		lv_display_get_driver_data(display);
	struct fplinux_drm_session *session = &driver->session;
	char error[160];

	if (driver->opened || connector_id != -1) {
		errno = EINVAL;
		display_error("display requires automatic connector selection");
	}
	if (!fplinux_drm_session_open(session, file, NULL, DRM_FORMAT_RGB565,
				      error, sizeof(error))) {
		fprintf(stderr, "MicroPythonOS display: %s\n", error);
		exit(EXIT_FAILURE);
	}
	driver->opened = true;
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_resolution(display, session->width, session->height);
	lv_display_set_buffers(display, session->mapping,
			       session->mapping + session->page_bytes,
			       session->page_bytes,
			       LV_DISPLAY_RENDER_MODE_FULL);
	if (!fplinux_drm_session_set_active_handler(session, set_active,
						    display))
		display_error("input activation");
	driver->timer = lv_timer_create(dispatch_vt, 10, driver);
	if (!driver->timer) {
		errno = ENOMEM;
		display_error("VT timer allocation");
	}
}
