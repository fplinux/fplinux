// SPDX-License-Identifier: GPL-2.0-only
#include "armada-scene.h"
#include "armada-gpu-renderer.h"
#include "armada-overlay.h"
#include "armada-renderer.h"
#include "armada-storyboard.h"
#include "fplinux-gpu-output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct armada_scene {
	struct armada_storyboard storyboard;
	struct armada_frame frame;
	struct armada_renderer *renderer;
	struct armada_gpu_renderer *gpu_renderer;
	unsigned int width;
	unsigned int height;
	uint16_t *pixels;
	const struct fplinux_font *font;
	char fallback_reason[512];
	char error[512];
};

struct armada_scene *armada_scene_create_with_renderer(
	unsigned int width, unsigned int height,
	const struct fplinux_font *font, struct fplinux_drm_session *display,
	enum armada_render_mode mode, char *error, size_t error_size)
{
	struct armada_scene *scene;
	bool cleanup_complete;

	if (!font || !font->bitmap || !font->width || !font->height ||
	    width == 0U || height == 0U || width > 320U || height > 320U ||
	    (size_t)width * height > 240U * 320U ||
	    (mode != ARMADA_RENDER_AUTO && mode != ARMADA_RENDER_SOFTWARE &&
	     mode != ARMADA_RENDER_GPU)) {
		errno = EINVAL;
		snprintf(error, error_size, "invalid ARMADA scene parameters");
		return NULL;
	}
	scene = calloc(1, sizeof(*scene));
	if (!scene) {
		snprintf(error, error_size, "cannot allocate scene: %s",
			 strerror(errno));
		return NULL;
	}
	scene->width = width;
	scene->height = height;
	scene->font = font;
	if (mode != ARMADA_RENDER_SOFTWARE) {
		scene->gpu_renderer = armada_gpu_renderer_create(
			display, width, height, &cleanup_complete, error,
			error_size);
		if (scene->gpu_renderer)
			return scene;
		if (mode == ARMADA_RENDER_GPU || !cleanup_complete) {
			free(scene);
			return NULL;
		}
		snprintf(scene->fallback_reason, sizeof(scene->fallback_reason),
			 "%s", error);
	}
	scene->pixels = calloc((size_t)width * height, sizeof(*scene->pixels));
	if (!scene->pixels) {
		snprintf(error, error_size,
			 "cannot allocate render surface: %s", strerror(errno));
		free(scene);
		return NULL;
	}
	scene->renderer =
		armada_renderer_create(width, height, scene->pixels, font);
	if (!scene->renderer) {
		snprintf(error, error_size,
			 "cannot allocate software renderer: %s",
			 strerror(errno));
		armada_scene_destroy(scene);
		return NULL;
	}
	return scene;
}

bool armada_scene_destroy(struct armada_scene *scene)
{
	bool success;

	if (!scene)
		return true;
	armada_renderer_destroy(scene->renderer);
	success = armada_gpu_renderer_destroy(scene->gpu_renderer);
	free(scene->pixels);
	free(scene);
	return success;
}

bool armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 const struct armada_metrics *metrics,
			 struct armada_outputs *outputs)
{
	if (!scene || !outputs)
		return false;
	armada_storyboard_update(&scene->storyboard, frame, &scene->frame,
				 outputs);
	if (scene->gpu_renderer) {
		struct fplinux_gpu_output *output = armada_scene_output(scene);
		void *pixels;
		size_t stride;

		if (!armada_gpu_renderer_render(scene->gpu_renderer,
						&scene->frame)) {
			snprintf(
				scene->error, sizeof(scene->error), "%s",
				armada_gpu_renderer_error(scene->gpu_renderer));
			return false;
		}
		if (!fplinux_gpu_output_begin_cpu(output, &pixels, &stride))
			goto output_failed;
		armada_overlay_draw_stride(scene->width, scene->height, pixels,
					   stride, scene->font,
					   scene->frame.number, metrics);
		if (!fplinux_gpu_output_end_cpu(output))
			goto output_failed;
	} else {
		armada_renderer_render(scene->renderer, &scene->frame, metrics);
	}
	return true;

output_failed:
	snprintf(scene->error, sizeof(scene->error), "%s",
		 fplinux_gpu_output_error(armada_scene_output(scene)));
	return false;
}

struct fplinux_gpu_output *armada_scene_output(const struct armada_scene *scene)
{
	return scene->gpu_renderer ?
		       armada_gpu_renderer_output(scene->gpu_renderer) :
		       NULL;
}

const char *armada_scene_renderer(const struct armada_scene *scene)
{
	return scene->gpu_renderer ?
		       armada_gpu_renderer_name(scene->gpu_renderer) :
		       "software";
}

const uint16_t *armada_scene_pixels(const struct armada_scene *scene)
{
	return scene->pixels;
}

const char *armada_scene_fallback_reason(const struct armada_scene *scene)
{
	return scene->fallback_reason;
}

const char *armada_scene_error(const struct armada_scene *scene)
{
	return scene->error;
}
