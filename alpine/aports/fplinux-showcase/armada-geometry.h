// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_GEOMETRY_H
#define FPLINUX_ARMADA_GEOMETRY_H

#include "armada-frame.h"

#define ARMADA_STROKES 20U
#define ARMADA_PRISM_VERTICES 12U
#define ARMADA_PRISM_FACES 20U
#define ARMADA_PRISM_PROFILE_POINTS 6U

struct armada_prepared_stroke {
	float midpoint_x;
	float midpoint_y;
	float unit_x;
	float unit_y;
	float half_length;
};

struct armada_glyph {
	uint8_t first;
	uint8_t count;
};

struct armada_prism_profile {
	float across;
	float depth;
};

extern const struct armada_glyph armada_glyphs[ARMADA_LETTERS];
extern const uint8_t armada_prism_faces[ARMADA_PRISM_FACES][3];
extern const struct armada_prism_profile
	armada_prism_profile[ARMADA_PRISM_PROFILE_POINTS];

void armada_prepare_stroke_geometry(
	struct armada_prepared_stroke prepared[ARMADA_STROKES]);
struct armada_vec3 armada_letter_base_color(unsigned int letter);

#endif
