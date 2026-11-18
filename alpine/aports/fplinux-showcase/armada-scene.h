// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_SCENE_H
#define FPLINUX_ARMADA_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARMADA_FRAMES_PER_SECOND 30U
#define ARMADA_DURATION_SECONDS 38U
#define ARMADA_DURATION_FRAMES \
	(ARMADA_FRAMES_PER_SECOND * ARMADA_DURATION_SECONDS)

struct armada_scene;

enum armada_render_mode {
	ARMADA_RENDER_AUTO,
	ARMADA_RENDER_SOFTWARE,
	ARMADA_RENDER_GPU,
};

struct armada_outputs {
	bool keypad;
	bool rumble;
	uint16_t cue_id;
	unsigned int rumble_remaining_ms;
	int lcd_level;
};

struct armada_scene *
armada_scene_create_with_renderer(unsigned int width, unsigned int height,
				  enum armada_render_mode mode, char *error,
				  size_t error_size);
bool armada_scene_destroy(struct armada_scene *scene);
bool armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 struct armada_outputs *outputs);
/* Borrowed tight RGB565 buffer for either renderer. */
const uint16_t *armada_scene_pixels(const struct armada_scene *scene);
const char *armada_scene_renderer(const struct armada_scene *scene);
enum armada_render_mode
armada_scene_render_mode(const struct armada_scene *scene);
const char *armada_scene_fallback_reason(const struct armada_scene *scene);
const char *armada_scene_error(const struct armada_scene *scene);

#endif
