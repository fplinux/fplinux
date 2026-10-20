// SPDX-License-Identifier: GPL-2.0-only
#include "armada-geometry.h"
#include "armada-math.h"

struct armada_stroke {
	int8_t ax;
	int8_t ay;
	int8_t bx;
	int8_t by;
};

static const struct armada_stroke strokes[ARMADA_STROKES] = {
	{ -30, -62, -30, 62 }, { -30, 62, 34, 62 },   { -30, 10, 25, 10 },
	{ -30, -62, -30, 62 }, { -30, 62, 30, 62 },   { 30, 12, 30, 62 },
	{ -30, 12, 30, 12 },   { -30, -62, -30, 62 }, { -30, -62, 34, -62 },
	{ -32, 62, 32, 62 },   { 0, -62, 0, 62 },     { -32, -62, 32, -62 },
	{ -31, -62, -31, 62 }, { 31, -62, 31, 62 },   { -31, 62, 31, -62 },
	{ -31, -60, -31, 62 }, { 31, -60, 31, 62 },   { -31, -60, 31, -60 },
	{ -33, -62, 33, 62 },  { -33, 62, 33, -62 },
};

const struct armada_glyph armada_glyphs[ARMADA_LETTERS] = {
	{ 0, 3 }, { 3, 4 }, { 7, 2 }, { 9, 3 }, { 12, 3 }, { 15, 3 }, { 18, 2 },
};

const uint8_t armada_prism_faces[ARMADA_PRISM_FACES][3] = {
	{ 0, 1, 7 }, { 0, 7, 6 },  { 1, 2, 8 },	 { 1, 8, 7 },  { 2, 3, 9 },
	{ 2, 9, 8 }, { 3, 4, 10 }, { 3, 10, 9 }, { 4, 5, 11 }, { 4, 11, 10 },
	{ 5, 0, 6 }, { 5, 6, 11 }, { 0, 2, 1 },	 { 0, 3, 2 },  { 0, 4, 3 },
	{ 0, 5, 4 }, { 6, 7, 8 },  { 6, 8, 9 },	 { 6, 9, 10 }, { 6, 10, 11 },
};

const struct armada_prism_profile
	armada_prism_profile[ARMADA_PRISM_PROFILE_POINTS] = {
		{ -0.062f, -0.170f }, { 0.062f, -0.170f }, { 0.088f, -0.142f },
		{ 0.088f, 0.170f },   { -0.088f, 0.170f }, { -0.088f, -0.142f },
	};

void armada_prepare_stroke_geometry(
	struct armada_prepared_stroke prepared[ARMADA_STROKES])
{
	unsigned int index;

	for (index = 0; index < ARMADA_STROKES; ++index) {
		const struct armada_stroke *stroke = &strokes[index];
		float ax = (float)stroke->ax / 100.0f;
		float ay = (float)stroke->ay / 100.0f;
		float bx = (float)stroke->bx / 100.0f;
		float by = (float)stroke->by / 100.0f;
		float dx = bx - ax;
		float dy = by - ay;
		float squared = dx * dx + dy * dy;
		float inverse = fast_inverse_sqrt(squared);
		float length = squared > 0.0000001f ? squared * inverse : 0.0f;

		prepared[index].unit_x = dx * inverse;
		prepared[index].unit_y = dy * inverse;
		prepared[index].midpoint_x = (ax + bx) * 0.5f;
		prepared[index].midpoint_y = (ay + by) * 0.5f;
		prepared[index].half_length = length * 0.5f;
	}
}

struct armada_vec3 armada_letter_base_color(unsigned int letter)
{
	static const struct armada_vec3 colors[ARMADA_LETTERS] = {
		{ 1.0f, 0.23137255f, 0.18823529f },
		{ 1.0f, 0.58431373f, 0.0f },
		{ 1.0f, 0.83921569f, 0.03921569f },
		{ 0.20392157f, 0.78039216f, 0.34901961f },
		{ 0.19607843f, 0.67843137f, 0.90196078f },
		{ 0.03921569f, 0.51764706f, 1.0f },
		{ 0.68627451f, 0.32156863f, 0.87058824f },
	};

	return colors[letter];
}
