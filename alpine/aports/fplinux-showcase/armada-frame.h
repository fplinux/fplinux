// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_FRAME_H
#define FPLINUX_ARMADA_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define ARMADA_LETTERS 7U
#define ARMADA_WATER_Y (-0.72f)

struct armada_vec3 {
	float x;
	float y;
	float z;
};

struct armada_letter_state {
	struct armada_vec3 position;
	float scale;
	float scale_x;
	float scale_y;
	float yaw;
	float pitch;
	float roll;
	float yaw_sine;
	float yaw_cosine;
	float pitch_sine;
	float pitch_cosine;
	float roll_sine;
	float roll_cosine;
	float emissive;
	bool visible;
};

struct armada_ripple {
	struct armada_vec3 origin;
	float radius;
	float amplitude;
};

struct armada_environment {
	struct armada_vec3 light_color;
	struct armada_vec3 celestial_direction;
	struct armada_vec3 sun_direction;
	struct armada_vec3 moon_direction;
	float sun_visibility;
	float moon_visibility;
	float daylight;
	float night;
	float exposure;
};

struct armada_frame {
	uint32_t number;
	struct armada_letter_state letters[ARMADA_LETTERS];
	struct armada_ripple ripples[ARMADA_LETTERS];
	unsigned int ripple_count;
	struct armada_environment environment;
};

#endif
