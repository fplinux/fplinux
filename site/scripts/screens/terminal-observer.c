/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-observer.h"
#include "terminal-pty.h"
#include "terminal-render.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-ui-screen.h>
#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static struct fplinux_ui_screen *canvas;
static struct fplinux_ui *application;
static struct fplinux_ui_screen *list;
static bool shell_resources_closed;
static bool pty_watched;
static bool final_output_visible;
static struct terminal_frame_observation terminal_observation;
static lv_timer_t *observer_timer;

static void observer_tick(lv_timer_t *timer)
{
	(void)timer;
}

void __real_fplinux_ui_watch_fd(struct fplinux_ui *ui, int fd, short events,
				fplinux_ui_fd_cb callback, void *context);

void __wrap_fplinux_ui_watch_fd(struct fplinux_ui *ui, int fd, short events,
				fplinux_ui_fd_cb callback, void *context)
{
	pty_watched = fd >= 0;
	__real_fplinux_ui_watch_fd(ui, fd, events, callback, context);
}

void __real_fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty);

void __wrap_fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty)
{
	__real_fplinux_terminal_pty_close(pty);
	shell_resources_closed = pty->fd == -1 && pty->shell == -1 &&
				 pty->supervisor == -1 && pty->owner_fd == -1 &&
				 pty->status_fd == -1;
}

void __real_fplinux_terminal_render(struct fplinux_terminal *terminal,
				    const struct fplinux_terminal_grid *grid,
				    lv_layer_t *layer, const lv_area_t *bounds,
				    lv_color_t accent, lv_color_t accent_text);

void __wrap_fplinux_terminal_render(struct fplinux_terminal *terminal,
				    const struct fplinux_terminal_grid *grid,
				    lv_layer_t *layer, const lv_area_t *bounds,
				    lv_color_t accent, lv_color_t accent_text)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);
	size_t count = tsm_screen_get_width(terminal->screen) *
		       tsm_screen_get_height(terminal->screen);
	char text[4096];

	assert(count < sizeof(text));
	for (size_t index = 0; index < count; ++index)
		text[index] = cells[index].ch >= 32 && cells[index].ch < 128 ?
				      (char)cells[index].ch :
				      ' ';
	text[count] = '\0';
	final_output_visible = strstr(text, "FINAL-SHELL-OUTPUT") != NULL;
	terminal_observation.terminal_columns =
		tsm_screen_get_width(terminal->screen);
	terminal_observation.terminal_rows =
		tsm_screen_get_height(terminal->screen);
	terminal_observation.terminal_cursor_x =
		tsm_screen_get_cursor_x(terminal->screen);
	terminal_observation.terminal_cursor_y =
		tsm_screen_get_cursor_y(terminal->screen);
	terminal_observation.terminal_flags =
		tsm_screen_get_flags(terminal->screen);
	terminal_observation.terminal_preedit =
		fplinux_terminal_preedit(terminal);
	terminal_observation.terminal_first_character = cells[0].ch;
	terminal_observation.terminal_first_background =
		(uint32_t)cells[0].bg.r << 16 | (uint32_t)cells[0].bg.g << 8 |
		cells[0].bg.b;
	terminal_observation.terminal_edge_character = cells[count - 1].ch;
	terminal_observation.terminal_cursor_visible = terminal->cursor_visible;
	__real_fplinux_terminal_render(terminal, grid, layer, bounds, accent,
				       accent_text);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_UI_LOCALE_DIRECTORY");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

struct fplinux_ui_screen *__real_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title);

struct fplinux_ui_screen *__wrap_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title)
{
	application = ui;
	canvas = __real_fplinux_ui_canvas_new(ui, title);
	/* A completed shell pauses its timer; capture requests still need a wakeup. */
	observer_timer = lv_timer_create(observer_tick, 16, NULL);
	assert(observer_timer);
	return canvas;
}

void __real_fplinux_ui_close(struct fplinux_ui *ui);

void __wrap_fplinux_ui_close(struct fplinux_ui *ui)
{
	if (observer_timer)
		lv_timer_delete(observer_timer);
	observer_timer = NULL;
	__real_fplinux_ui_close(ui);
}

struct fplinux_ui_screen *
__real_fplinux_ui_list_new(struct fplinux_ui *ui, const char *title,
			   enum fplinux_ui_list_kind kind);

struct fplinux_ui_screen *
__wrap_fplinux_ui_list_new(struct fplinux_ui *ui, const char *title,
			   enum fplinux_ui_list_kind kind)
{
	list = __real_fplinux_ui_list_new(ui, title, kind);
	return list;
}

static void collect_labels(lv_obj_t *object,
			   struct terminal_frame_observation *observation)
{
	if (!lv_obj_is_visible(object))
		return;
	if (lv_obj_check_type(object, &lv_label_class)) {
		struct terminal_label_observation *label;
		const lv_font_t *font =
			lv_obj_get_style_text_font(object, LV_PART_MAIN);
		lv_label_long_mode_t mode = lv_label_get_long_mode(object);
		lv_area_t bounds;
		lv_point_t size;

		if (observation->label_count ==
		    sizeof(observation->labels) /
			    sizeof(observation->labels[0])) {
			observation->labels_truncated = true;
			return;
		}
		label = &observation->labels[observation->label_count++];
		snprintf(label->text, sizeof(label->text), "%s",
			 lv_label_get_text(object));
		lv_obj_get_coords(object, &bounds);
		label->left = bounds.x1;
		label->top = bounds.y1;
		label->width = lv_area_get_width(&bounds);
		label->height = lv_area_get_height(&bounds);
		lv_text_get_size(&size, label->text, font,
				 lv_obj_get_style_text_letter_space(
					 object, LV_PART_MAIN),
				 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		label->text_width = size.x;
		label->text_height = size.y;
		label->scrolls = mode == LV_LABEL_LONG_MODE_SCROLL ||
				 mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
		label->ellipsizes = mode == LV_LABEL_LONG_MODE_DOTS;
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		collect_labels(lv_obj_get_child(object, (int32_t)index),
			       observation);
}

static void observe_frame(struct terminal_frame_observation *observation)
{
	lv_display_t *display = lv_display_get_default();
	lv_obj_t *content = fplinux_ui_canvas_object(canvas);
	lv_area_t bounds;

	assert(display && content);
	observation->shell_resources_closed = shell_resources_closed;
	observation->pty_watched = pty_watched;
	observation->final_output_visible = final_output_visible;
	observation->terminal_columns = terminal_observation.terminal_columns;
	observation->terminal_rows = terminal_observation.terminal_rows;
	observation->terminal_cursor_x = terminal_observation.terminal_cursor_x;
	observation->terminal_cursor_y = terminal_observation.terminal_cursor_y;
	observation->terminal_flags = terminal_observation.terminal_flags;
	observation->terminal_preedit = terminal_observation.terminal_preedit;
	observation->terminal_first_character =
		terminal_observation.terminal_first_character;
	observation->terminal_first_background =
		terminal_observation.terminal_first_background;
	observation->terminal_edge_character =
		terminal_observation.terminal_edge_character;
	observation->terminal_cursor_visible =
		terminal_observation.terminal_cursor_visible;
	lv_obj_get_coords(content, &bounds);
	observation->content_left = bounds.x1;
	observation->content_top = bounds.y1;
	observation->content_width = lv_area_get_width(&bounds);
	observation->content_height = lv_area_get_height(&bounds);
	if (list && fplinux_ui_current(application) == list) {
		observation->list_count = fplinux_ui_list_size(list);
		observation->list_focused = fplinux_ui_list_get_focused(list);
		for (size_t index = 0;
		     index < 2 && index < observation->list_count; ++index)
			observation->list_selected[index] =
				fplinux_ui_list_is_selected(list, index);
	}
	collect_labels(lv_display_get_screen_active(display), observation);
	collect_labels(lv_display_get_layer_top(display), observation);
	collect_labels(lv_display_get_layer_sys(display), observation);
}

int __real_wl_display_flush(struct wl_display *display);

static void frame_completed(void *data, struct wl_callback *callback,
			    uint32_t serial)
{
	struct terminal_frame_observation observation = { 0 };

	(void)data;
	(void)serial;
	assert(phase == CAPTURE_COMPLETE);
	wl_callback_destroy(callback);
	observe_frame(&observation);
	assert(send(observer, &observation, sizeof(observation), 0) ==
	       (ssize_t)sizeof(observation));
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
