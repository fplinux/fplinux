/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <wayland-client.h>

enum capture_phase {
	CAPTURE_IDLE,
	CAPTURE_INPUT,
	CAPTURE_RENDER,
	CAPTURE_COMMIT,
	CAPTURE_COMPLETE,
};

static enum capture_phase phase;
static int observer;
static lv_event_dsc_t *render_observation;

int __real_wl_display_flush(struct wl_display *display);

static void frame_completed(void *data, struct wl_callback *callback,
			    uint32_t serial)
{
	char ready = 'F';

	(void)data;
	(void)serial;
	assert(phase == CAPTURE_COMPLETE);
	wl_callback_destroy(callback);
	assert(send(observer, &ready, 1, 0) == 1);
	phase = CAPTURE_IDLE;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_completed,
};

static void rendered(lv_event_t *event)
{
	(void)event;
	if (phase == CAPTURE_RENDER)
		phase = CAPTURE_COMMIT;
}

static void input_dispatched(void *data, struct wl_callback *callback,
			     uint32_t serial)
{
	lv_display_t *display = lv_display_get_default();

	(void)data;
	(void)serial;
	assert(display && phase == CAPTURE_INPUT);
	wl_callback_destroy(callback);
	/* Keys sent before the sync reply have passed through the real handlers. */
	phase = CAPTURE_RENDER;
	lv_obj_invalidate(lv_display_get_screen_active(display));
}

static const struct wl_callback_listener input_listener = {
	.done = input_dispatched,
};

int __wrap_wl_display_flush(struct wl_display *display)
{
	struct wl_callback *callback;
	char request;
	ssize_t received;

	if (!observer) {
		const char *descriptor = getenv("FPLINUX_TERMINAL_OBSERVER_FD");

		assert(descriptor);
		observer = atoi(descriptor);
		assert(observer > 0);
	}
	if (phase == CAPTURE_IDLE) {
		received = recv(observer, &request, 1, MSG_DONTWAIT);
		if (received < 0)
			assert(errno == EAGAIN || errno == EWOULDBLOCK);
		else {
			lv_display_t *lv_display = lv_display_get_default();

			assert(received == 1 && request == 'F' && lv_display);
			if (!render_observation) {
				render_observation = lv_display_add_event_cb(
					lv_display, rendered,
					LV_EVENT_RENDER_READY, NULL);
				assert(render_observation);
			}
			phase = CAPTURE_INPUT;
			callback = wl_display_sync(display);
			assert(callback);
			assert(wl_callback_add_listener(
				       callback, &input_listener, NULL) == 0);
		}
	} else if (phase == CAPTURE_COMMIT) {
		/* The UI submitted the rendered canvas before entering this flush. */
		phase = CAPTURE_COMPLETE;
		callback = wl_display_sync(display);
		assert(callback);
		assert(wl_callback_add_listener(callback, &frame_listener,
						NULL) == 0);
	}
	return __real_wl_display_flush(display);
}
