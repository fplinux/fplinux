// SPDX-License-Identifier: GPL-2.0-only
#include "armada-scene.h"
#include "armada-gpu-renderer.h"
#include "armada-renderer.h"
#include "armada-storyboard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct armada_scene {
	struct armada_storyboard storyboard;
	struct armada_frame frame;
	struct armada_renderer *renderer;
	struct armada_gpu_renderer *gpu_renderer;
	uint16_t *pixels;
	char fallback_reason[512];
	char error[512];
};

struct armada_scene *
armada_scene_create_with_renderer(unsigned int width, unsigned int height,
				  enum armada_render_mode mode, char *error,
				  size_t error_size)
{
	struct armada_scene *scene;

	if (width == 0U || height == 0U || width > 320U || height > 320U ||
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
	scene->pixels = calloc((size_t)width * height, sizeof(*scene->pixels));
	if (!scene->pixels) {
		snprintf(error, error_size,
			 "cannot allocate render surface: %s", strerror(errno));
		free(scene);
		return NULL;
	}
	if (mode != ARMADA_RENDER_SOFTWARE) {
		scene->gpu_renderer = armada_gpu_renderer_create(
			width, height, scene->pixels, error, error_size);
		if (scene->gpu_renderer)
			return scene;
		if (mode == ARMADA_RENDER_GPU) {
			armada_scene_destroy(scene);
			return NULL;
		}
		snprintf(scene->fallback_reason, sizeof(scene->fallback_reason),
			 "%s", error);
	}
	scene->renderer = armada_renderer_create(width, height, scene->pixels);
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
			 struct armada_outputs *outputs)
{
	if (!scene || !outputs)
		return false;
	armada_storyboard_update(&scene->storyboard, frame, &scene->frame,
				 outputs);
	if (scene->gpu_renderer) {
		if (!armada_gpu_renderer_render(scene->gpu_renderer,
						&scene->frame)) {
			snprintf(
				scene->error, sizeof(scene->error), "%s",
				armada_gpu_renderer_error(scene->gpu_renderer));
			return false;
		}
	} else {
		armada_renderer_render(scene->renderer, &scene->frame);
	}
	return true;
}

const char *armada_scene_renderer(const struct armada_scene *scene)
{
	return scene->gpu_renderer ?
		       armada_gpu_renderer_name(scene->gpu_renderer) :
		       "software";
}

enum armada_render_mode
armada_scene_render_mode(const struct armada_scene *scene)
{
	return scene->gpu_renderer ? ARMADA_RENDER_GPU : ARMADA_RENDER_SOFTWARE;
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
