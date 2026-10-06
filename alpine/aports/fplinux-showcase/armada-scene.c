// SPDX-License-Identifier: GPL-2.0-only
#include "armada-scene.h"
#include "armada-renderer.h"
#include "armada-storyboard.h"

#include <errno.h>
#include <stdlib.h>

struct armada_scene {
	struct armada_storyboard storyboard;
	struct armada_frame frame;
	struct armada_renderer *renderer;
};

struct armada_scene *armada_scene_create(unsigned int width,
					 unsigned int height, uint16_t *pixels,
					 const struct fplinux_font *font)
{
	struct armada_scene *scene;

	if (!pixels || !font || !font->bitmap || !font->width ||
	    !font->height || width == 0U || height == 0U || width > 240U ||
	    height > 320U) {
		errno = EINVAL;
		return NULL;
	}
	scene = calloc(1, sizeof(*scene));
	if (!scene)
		return NULL;
	scene->renderer = armada_renderer_create(width, height, pixels, font);
	if (!scene->renderer) {
		free(scene);
		return NULL;
	}
	return scene;
}

void armada_scene_destroy(struct armada_scene *scene)
{
	if (!scene)
		return;
	armada_renderer_destroy(scene->renderer);
	free(scene);
}

void armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 const struct armada_metrics *metrics,
			 struct armada_outputs *outputs)
{
	if (!scene || !outputs)
		return;
	armada_storyboard_update(&scene->storyboard, frame, &scene->frame,
				 outputs);
	armada_renderer_render(scene->renderer, &scene->frame, metrics);
}
