// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_SCENE_H
#define FPLINUX_ARMADA_SCENE_H

#include "fplinux-font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARMADA_FRAMES_PER_SECOND 30U
#define ARMADA_DURATION_SECONDS 38U
#define ARMADA_DURATION_FRAMES \
	(ARMADA_FRAMES_PER_SECOND * ARMADA_DURATION_SECONDS)

struct armada_scene;
struct fplinux_drm_session;
struct fplinux_gpu_output;

enum armada_render_mode {
	ARMADA_RENDER_AUTO,
	ARMADA_RENDER_SOFTWARE,
	ARMADA_RENDER_GPU,
};

struct armada_metrics {
	uint32_t average_fps_tenths;
	uint32_t minimum_fps_tenths;
	uint32_t maximum_frame_us;
	bool valid;
};

struct armada_outputs {
	bool keypad;
	bool rumble;
	uint16_t cue_id;
	int lcd_level;
};

struct armada_scene *armada_scene_create_with_renderer(
	unsigned int width, unsigned int height,
	const struct fplinux_font *font, struct fplinux_drm_session *display,
	enum armada_render_mode mode, char *error, size_t error_size);
bool armada_scene_destroy(struct armada_scene *scene);
bool armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 const struct armada_metrics *metrics,
			 struct armada_outputs *outputs);
/* Borrowed native output; presentation remains with the display owner. */
struct fplinux_gpu_output *
armada_scene_output(const struct armada_scene *scene);
/* Borrowed tight software buffer, or NULL for native GPU output. */
const uint16_t *armada_scene_pixels(const struct armada_scene *scene);
const char *armada_scene_renderer(const struct armada_scene *scene);
const char *armada_scene_fallback_reason(const struct armada_scene *scene);
const char *armada_scene_error(const struct armada_scene *scene);

#endif
