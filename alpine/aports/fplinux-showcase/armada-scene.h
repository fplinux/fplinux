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

struct armada_scene *armada_scene_create(unsigned int width,
					 unsigned int height, uint16_t *pixels);
void armada_scene_destroy(struct armada_scene *scene);
void armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 const struct armada_metrics *metrics,
			 struct armada_outputs *outputs);

#endif
