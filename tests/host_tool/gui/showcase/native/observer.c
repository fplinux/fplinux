/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "observer.h"
#include "armada-scene.h"
#include "armada-gpu-renderer.h"
#include "fplinux-language.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-frame.h>
#include <assert.h>
#include <errno.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static struct fplinux_ui *application_ui;
static struct fplinux_ui_screen *canvas;
static struct fplinux_ui_frame *application_frame;
static struct showcase_observation observed;
static struct showcase_control control = { .now_ms = 1000 };
static unsigned int live_scenes;
static uint64_t measured_ns = 1000000000ULL;

FILE *__real_fopen(const char *path, const char *mode);

FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (!strcmp(path, "/etc/os-release"))
		path = getenv("FPLINUX_SHOWCASE_OS_RELEASE");
	else if (!strcmp(path, "/proc/device-tree/model"))
		path = getenv("FPLINUX_SHOWCASE_MODEL");
	assert(path);
	return __real_fopen(path, mode);
}

static int observer(void)
{
	const char *descriptor = getenv("FPLINUX_SHOWCASE_OBSERVER_FD");

	assert(descriptor);
	return atoi(descriptor);
}

static void receive_control(bool wait)
{
	struct showcase_control update;
	ssize_t bytes;

	while ((bytes = recv(observer(), &update, sizeof(update),
			     wait ? 0 : MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(update));
		assert(update.now_ms >= control.now_ms);
		measured_ns += update.advance_ns;
		update.present_permits += control.present_permits;
		control = update;
		if (wait)
			return;
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

uint64_t __wrap_fplinux_ui_now_ms(void)
{
	receive_control(false);
	return control.now_ms;
}

int showcase_observer_clock_gettime(clockid_t clock, struct timespec *time)
{
	receive_control(false);
	if (clock != CLOCK_MONOTONIC || !control.measured_clock)
		return clock_gettime(clock, time);
	time->tv_sec = (time_t)(measured_ns / 1000000000ULL);
	time->tv_nsec = (long)(measured_ns % 1000000000ULL);
	return 0;
}

static void notify(enum showcase_observation_kind kind)
{
	observed.kind = kind;
	assert(send(observer(), &observed, sizeof(observed), MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(observed));
}

struct fplinux_ui *__real_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context);

struct fplinux_ui *__wrap_fplinux_ui_open(const char *app_id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_ui = __real_fplinux_ui_open(app_id, callback, context);
	return application_ui;
}

struct fplinux_ui_screen *__real_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title);

struct fplinux_ui_screen *__wrap_fplinux_ui_canvas_new(struct fplinux_ui *ui,
						       const char *title)
{
	assert(!title || !strcmp(title, "ARMADA"));
	canvas = __real_fplinux_ui_canvas_new(ui, title);
	return canvas;
}

struct fplinux_ui_frame *
__real_fplinux_ui_frame_new(struct fplinux_ui *ui, int width, int height,
			    enum fplinux_ui_frame_mode mode);

struct fplinux_ui_frame *
__wrap_fplinux_ui_frame_new(struct fplinux_ui *ui, int width, int height,
			    enum fplinux_ui_frame_mode mode)
{
	application_frame =
		__real_fplinux_ui_frame_new(ui, width, height, mode);
	return application_frame;
}

void __real_fplinux_ui_frame_delete(struct fplinux_ui_frame *frame);

void __wrap_fplinux_ui_frame_delete(struct fplinux_ui_frame *frame)
{
	if (frame == application_frame) {
		application_frame = NULL;
		canvas = NULL;
	}
	__real_fplinux_ui_frame_delete(frame);
}

bool __real_fplinux_ui_frame_present(struct fplinux_ui_frame *frame);

bool __wrap_fplinux_ui_frame_present(struct fplinux_ui_frame *frame)
{
	/* Pause after the App has measured rendering, before presentation. */
	notify(SHOWCASE_PRESENT_REQUEST);
	receive_control(false);
	while (!control.present_permits)
		receive_control(true);
	--control.present_permits;
	return __real_fplinux_ui_frame_present(frame);
}

struct armada_scene *__real_armada_scene_create_with_renderer(
	unsigned int width, unsigned int height, enum armada_render_mode mode,
	char *error, size_t error_size);

struct armada_scene *__wrap_armada_scene_create_with_renderer(
	unsigned int width, unsigned int height, enum armada_render_mode mode,
	char *error, size_t error_size)
{
	struct armada_scene *scene;

	observed.width = width;
	observed.height = height;
	observed.mode = (unsigned int)mode;
	++observed.created;
	scene = __real_armada_scene_create_with_renderer(width, height, mode,
							 error, error_size);
	if (scene) {
		++live_scenes;
		observed.actual_mode =
			(unsigned int)armada_scene_render_mode(scene);
	}
	return scene;
}

bool __real_armada_scene_destroy(struct armada_scene *scene);

bool __wrap_armada_scene_destroy(struct armada_scene *scene)
{
	if (scene) {
		assert(live_scenes);
		--live_scenes;
		++observed.destroyed;
	}
	return __real_armada_scene_destroy(scene);
}

void __real_fplinux_ui_close(struct fplinux_ui *ui);

void __wrap_fplinux_ui_close(struct fplinux_ui *ui)
{
	assert(live_scenes == 0);
	assert(!application_frame);
	notify(SHOWCASE_CLOSE);
	__real_fplinux_ui_close(ui);
}

bool __real_armada_scene_render(struct armada_scene *scene, uint32_t frame,
				struct armada_outputs *outputs);

bool __wrap_armada_scene_render(struct armada_scene *scene, uint32_t frame,
				struct armada_outputs *outputs)
{
	bool result;

	result = __real_armada_scene_render(scene, frame, outputs);
	observed.render_ns = 0;
	if (control.render_timing) {
		/* A supplied submillisecond duration, independent of App timing. */
		observed.render_ns = observed.rendered % 2 ? 750000 : 250000;
		measured_ns += observed.render_ns;
	}
	++observed.rendered;
	observed.frame = frame;
	notify(SHOWCASE_RENDER);
	return result;
}

/* The real software scene is linked. This double controls the optional GPU
 * boundary; renderer/readback correctness has separate production tests. */
struct armada_gpu_renderer {
	uint16_t *pixels;
	size_t count;
};

struct armada_gpu_renderer *
armada_gpu_renderer_create(unsigned int width, unsigned int height,
			   uint16_t *pixels, char *error, size_t error_size)
{
	struct armada_gpu_renderer *renderer;

	if (!getenv("FPLINUX_SHOWCASE_TEST_GPU")) {
		errno = ENODEV;
		snprintf(error, error_size, "test GPU unavailable");
		return NULL;
	}
	renderer = calloc(1, sizeof(*renderer));
	assert(renderer);
	renderer->pixels = pixels;
	renderer->count = (size_t)width * height;
	return renderer;
}

bool armada_gpu_renderer_render(struct armada_gpu_renderer *renderer,
				const struct armada_frame *frame)
{
	if (control.gpu_failure)
		return false;
	for (size_t index = 0; index < renderer->count; ++index)
		renderer->pixels[index] = (uint16_t)(index + frame->number);
	return true;
}

const char *armada_gpu_renderer_name(const struct armada_gpu_renderer *renderer)
{
	assert(renderer);
	return "test GPU";
}

const char *
armada_gpu_renderer_error(const struct armada_gpu_renderer *renderer)
{
	assert(renderer);
	return "test GPU render failure";
}

bool armada_gpu_renderer_destroy(struct armada_gpu_renderer *renderer)
{
	free(renderer);
	return true;
}

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_NATIVE_LANGUAGE_DIR");

	if (languages && directory &&
	    !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY))
		directory = languages;
	return __real_fplinux_language_load(directory, code, language);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_SHOWCASE_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}

static void labels(lv_obj_t *object, size_t *used)
{
	if (lv_obj_is_hidden(object))
		return;
	if (lv_obj_check_type(object, &lv_image_class))
		++observed.images;
	if (lv_obj_check_type(object, &lv_label_class)) {
		const char *text = lv_label_get_text(object);
		size_t length = strlen(text);
		const char *setup_labels[] = {
			"Start",
			"Renderer",
			"Screen mode",
			"Cycles",
		};
		lv_area_t bounds;

		lv_obj_get_coords(object, &bounds);
		for (size_t index = 0; index < 4; ++index)
			if (!strcmp(text, setup_labels[index]))
				observed.setup_y[index] = bounds.y1;

		assert(*used + length + 2 < sizeof(observed.labels));
		memcpy(observed.labels + *used, text, length);
		*used += length;
		observed.labels[(*used)++] = '\n';
		observed.labels[*used] = '\0';
		if (!strcmp(text, "ARMADA")) {
			assert(bounds.x1 >= 0 && bounds.y1 >= 0);
			assert(bounds.x2 < fplinux_ui_width(application_ui));
			assert(bounds.y2 < fplinux_ui_height(application_ui));
		}
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		labels(lv_obj_get_child(object, (int32_t)index), used);
}

void __real_lv_display_flush_ready(lv_display_t *display);

void __wrap_lv_display_flush_ready(lv_display_t *display)
{
	int saved_errno = errno;

	if (application_ui && lv_display_flush_is_last(display)) {
		size_t used = 1;
		struct fplinux_ui_screen *current =
			fplinux_ui_current(application_ui);

		observed.labels[0] = '\n';
		observed.images = 0;
		observed.labels[1] = '\0';
		++observed.paints;
		observed.canvas = fplinux_ui_current(application_ui) == canvas;
		observed.scene_visible =
			observed.canvas && application_frame &&
			lv_obj_is_visible(
				fplinux_ui_frame_object(application_frame));
		observed.fullscreen = fplinux_ui_is_fullscreen(application_ui);
		observed.focus = fplinux_ui_list_get_focused(current);
		observed.first_visible =
			fplinux_ui_list_get_first_visible(current);
		observed.rows = fplinux_ui_list_size(current);
		observed.chosen = 0;
		for (size_t index = 0; index < 4; ++index)
			observed.setup_y[index] = INT_MIN;
		if (application_frame) {
			lv_area_t bounds;

			lv_obj_get_coords(
				fplinux_ui_frame_object(application_frame),
				&bounds);
			observed.frame_x = bounds.x1;
			observed.frame_y = bounds.y1;
			observed.frame_width = lv_area_get_width(&bounds);
			observed.frame_height = lv_area_get_height(&bounds);
		}
		for (size_t index = 0; index < 6; ++index)
			if (fplinux_ui_list_is_selected(current, index))
				observed.chosen |= 1U << index;
		labels(lv_display_get_screen_active(display), &used);
		labels(lv_display_get_layer_top(display), &used);
		labels(lv_display_get_layer_sys(display), &used);
		notify(SHOWCASE_PAINT);
	}
	__real_lv_display_flush_ready(display);
	errno = saved_errno;
}
