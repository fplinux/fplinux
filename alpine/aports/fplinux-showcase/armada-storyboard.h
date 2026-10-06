// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_STORYBOARD_H
#define FPLINUX_ARMADA_STORYBOARD_H

#include "armada-frame.h"
#include "armada-scene.h"

struct armada_water_crossing {
	struct armada_vec3 position;
	uint32_t tick;
};

struct armada_letter_motion {
	struct armada_letter_state pose;
	struct armada_vec3 velocity;
	struct armada_vec3 angular_velocity;
	float stretch_velocity_x;
	float stretch_velocity_y;
	float recoil;
	float recoil_velocity;
	struct armada_water_crossing rise;
	struct armada_water_crossing sink;
};

struct armada_storyboard {
	struct armada_letter_motion motion[ARMADA_LETTERS];
	uint32_t motion_tick;
	bool motion_initialized;
};

void armada_storyboard_update(struct armada_storyboard *storyboard,
			      uint32_t number, struct armada_frame *frame,
			      struct armada_outputs *outputs);

#endif
