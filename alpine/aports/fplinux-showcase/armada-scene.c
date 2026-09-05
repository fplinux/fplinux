// SPDX-License-Identifier: GPL-2.0-only
#include "armada-scene.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARMADA_LETTERS 7U
#define ARMADA_STROKES 20U
#define ARMADA_NEAR_WATER_DISTANCE 13.0f
#define ARMADA_WATER_FAR_DISTANCE 80.0f
#define ARMADA_WATER_Y (-0.72f)
#define ARMADA_CLOUD_HEIGHT 7.2f
#define ARMADA_WORD_STEP 1.02f
#define ARMADA_LETTER_SCALE 1.06f
#define ARMADA_WORD_Y 0.28f
#define ARMADA_SUN_RADIUS 0.046f
#define ARMADA_MOON_RADIUS 0.040f
#define ARMADA_HARDWARE_FRAME (24U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_FINAL_METRICS (32U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_LETTER_STAGGER_FRAMES 5U
#define ARMADA_RISE_DURATION_FRAMES 36U
#define ARMADA_SINK_DURATION_FRAMES 45U
#define ARMADA_MOTION_STEPS_PER_FRAME 4U
#define ARMADA_SUBMERGED_FRAME (36U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_SINK_FRAME                                       \
	(ARMADA_SUBMERGED_FRAME - ARMADA_SINK_DURATION_FRAMES - \
	 (ARMADA_LETTERS - 1U) * ARMADA_LETTER_STAGGER_FRAMES)
#define ARMADA_RISE_RIPPLE_DURATION 84U
#define ARMADA_BACKGROUND_SCALE 4U
#define ARMADA_TRIANGLE_TILE_SIDE 8
#define ARMADA_MORSE_START_FRAME ARMADA_HARDWARE_FRAME
#define ARMADA_MORSE_UNIT_FRAMES 3U
#define ARMADA_GLASS_IOR 1.50f
#define ARMADA_GLASS_ABSORPTION 0.72f
#define ARMADA_SHADOW_SIDE 128U
#define ARMADA_LIGHT_SAMPLES 12U

struct armada_vec3 {
	float x;
	float y;
	float z;
};

struct armada_prepared_stroke {
	float midpoint_x;
	float midpoint_y;
	float unit_x;
	float unit_y;
	float half_length;
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

struct armada_ripple {
	struct armada_vec3 origin;
	float radius;
	float amplitude;
};

struct armada_shadow_bounds {
	float minimum_u;
	float maximum_u;
	float minimum_v;
	float maximum_v;
	float maximum_depth;
};

struct armada_scene {
	unsigned int width;
	unsigned int height;
	unsigned int low_width;
	unsigned int low_height;
	uint16_t *pixels;
	uint16_t *low_pixels;
	uint16_t *background_pixels;
	float *shadow_depth;
	struct armada_vec3 shadow_right;
	struct armada_vec3 shadow_up;
	struct armada_vec3 shadow_direction;
	struct armada_vec3 shadow_filter[ARMADA_LETTERS];
	struct armada_shadow_bounds shadow_bounds[ARMADA_LETTERS];
	float shadow_light_slopes[ARMADA_LIGHT_SAMPLES][2];
	float shadow_max_slope_u;
	float shadow_max_slope_v;
	float shadow_min_u;
	float shadow_min_v;
	float shadow_scale_u;
	float shadow_scale_v;
	float shadow_strength;
	struct armada_vec3 *rays;
	struct armada_vec3 *full_rays;
	float *triangle_depth;
	float *reflection_depth;
	float *water_surface_z;
	struct armada_prepared_stroke prepared_strokes[ARMADA_STROKES];
	uint32_t frame;
	struct armada_letter_state letters[ARMADA_LETTERS];
	struct armada_letter_motion motion[ARMADA_LETTERS];
	uint32_t motion_tick;
	bool motion_initialized;
	struct armada_ripple ripples[ARMADA_LETTERS];
	unsigned int ripple_count;
	struct armada_vec3 light_color;
	struct armada_vec3 celestial_direction;
	struct armada_vec3 sun_direction;
	struct armada_vec3 moon_direction;
	float sun_visibility;
	float moon_visibility;
	float daylight;
	float night;
	float exposure;
	float cloud_transmittance;
};

struct armada_morse_symbol {
	unsigned int letter;
	uint16_t cue_id;
	unsigned int elapsed;
	unsigned int duration;
};

struct armada_stroke {
	int8_t ax;
	int8_t ay;
	int8_t bx;
	int8_t by;
};

struct armada_glyph {
	uint8_t first;
	uint8_t count;
};

struct armada_water_sample {
	float height;
	float gradient_x;
	float gradient_z;
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

static const struct armada_glyph glyphs[ARMADA_LETTERS] = {
	{ 0, 3 }, { 3, 4 }, { 7, 2 }, { 9, 3 }, { 12, 3 }, { 15, 3 }, { 18, 2 },
};

static const char *const morse_patterns[ARMADA_LETTERS] = {
	"..-.", ".--.", ".-..", "..", "-.", "..-", "-..-",
};

struct armada_prism_profile {
	float across;
	float depth;
};

static const uint8_t stroke_prism_faces[20][3] = {
	{ 0, 1, 7 }, { 0, 7, 6 },  { 1, 2, 8 },	 { 1, 8, 7 },  { 2, 3, 9 },
	{ 2, 9, 8 }, { 3, 4, 10 }, { 3, 10, 9 }, { 4, 5, 11 }, { 4, 11, 10 },
	{ 5, 0, 6 }, { 5, 6, 11 }, { 0, 2, 1 },	 { 0, 3, 2 },  { 0, 4, 3 },
	{ 0, 5, 4 }, { 6, 7, 8 },  { 6, 8, 9 },	 { 6, 9, 10 }, { 6, 10, 11 },
};

static const struct armada_prism_profile stroke_prism_profile[6] = {
	{ -0.062f, -0.170f }, { 0.062f, -0.170f }, { 0.088f, -0.142f },
	{ 0.088f, 0.170f },   { -0.088f, 0.170f }, { -0.088f, -0.142f },
};

static float f_abs(float value)
{
	return value < 0.0f ? -value : value;
}
static float f_min(float first, float second)
{
	return first < second ? first : second;
}
static float f_max(float first, float second)
{
	return first > second ? first : second;
}
static float f_clamp(float value, float minimum, float maximum)
{
	return f_min(f_max(value, minimum), maximum);
}
static float smoothstep(float value)
{
	value = f_clamp(value, 0.0f, 1.0f);
	return value * value * (3.0f - 2.0f * value);
}

static bool morse_symbol_for_frame(uint32_t frame,
				   struct armada_morse_symbol *symbol)
{
	uint32_t elapsed;
	uint16_t cue_id = 0;
	unsigned int letter;

	*symbol = (struct armada_morse_symbol){ .letter = ARMADA_LETTERS };
	if (frame < ARMADA_MORSE_START_FRAME)
		return false;
	elapsed = frame - ARMADA_MORSE_START_FRAME;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const char *pattern = morse_patterns[letter];
		unsigned int index;

		for (index = 0; pattern[index]; ++index) {
			unsigned int duration =
				(pattern[index] == '-' ? 3U : 1U) *
				ARMADA_MORSE_UNIT_FRAMES;

			if (elapsed < duration) {
				symbol->letter = letter;
				symbol->cue_id = (uint16_t)(cue_id + 1U);
				symbol->elapsed = elapsed;
				symbol->duration = duration;
				return true;
			}
			elapsed -= duration;
			++cue_id;
			if (pattern[index + 1U]) {
				if (elapsed < ARMADA_MORSE_UNIT_FRAMES)
					return false;
				elapsed -= ARMADA_MORSE_UNIT_FRAMES;
			}
		}
		if (letter + 1U < ARMADA_LETTERS) {
			unsigned int gap = 3U * ARMADA_MORSE_UNIT_FRAMES;

			if (elapsed < gap)
				return false;
			elapsed -= gap;
		}
	}
	return false;
}

static struct armada_vec3 vec3(float x, float y, float z)
{
	struct armada_vec3 result = { .x = x, .y = y, .z = z };
	return result;
}
static struct armada_vec3 vec_add(struct armada_vec3 a, struct armada_vec3 b)
{
	return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static struct armada_vec3 vec_sub(struct armada_vec3 a, struct armada_vec3 b)
{
	return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static struct armada_vec3 vec_scale(struct armada_vec3 v, float s)
{
	return vec3(v.x * s, v.y * s, v.z * s);
}
static float vec_dot(struct armada_vec3 a, struct armada_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static float fast_inverse_sqrt(float value)
{
	uint32_t bits;
	float estimate;
	float half;

	if (value <= 0.0000001f)
		return 0.0f;
	memcpy(&bits, &value, sizeof(bits));
	bits = UINT32_C(0x5f3759df) - (bits >> 1);
	memcpy(&estimate, &bits, sizeof(estimate));
	half = value * 0.5f;
	estimate *= 1.5f - half * estimate * estimate;
	estimate *= 1.5f - half * estimate * estimate;
	return estimate;
}
static float vec_length(struct armada_vec3 v)
{
	float squared = vec_dot(v, v);
	return squared > 0.0000001f ? squared * fast_inverse_sqrt(squared) :
				      0.0f;
}
static struct armada_vec3 vec_normalize(struct armada_vec3 v)
{
	float inverse = fast_inverse_sqrt(vec_dot(v, v));
	return inverse > 0.0f ? vec_scale(v, inverse) : vec3(0.0f, 1.0f, 0.0f);
}
static struct armada_vec3 vec_reflect(struct armada_vec3 direction,
				      struct armada_vec3 normal)
{
	return vec_sub(direction,
		       vec_scale(normal, 2.0f * vec_dot(direction, normal)));
}
static struct armada_vec3 color_mix(struct armada_vec3 a, struct armada_vec3 b,
				    float weight)
{
	weight = f_clamp(weight, 0.0f, 1.0f);
	return vec_add(vec_scale(a, 1.0f - weight), vec_scale(b, weight));
}
static struct armada_vec3 color_filter(struct armada_vec3 color,
				       struct armada_vec3 transmission)
{
	return vec3(color.x * transmission.x, color.y * transmission.y,
		    color.z * transmission.z);
}
static uint16_t color_to_rgb565(struct armada_vec3 color)
{
	unsigned int red =
		(unsigned int)(f_clamp(color.x, 0.0f, 1.0f) * 255.0f);
	unsigned int green =
		(unsigned int)(f_clamp(color.y, 0.0f, 1.0f) * 255.0f);
	unsigned int blue =
		(unsigned int)(f_clamp(color.z, 0.0f, 1.0f) * 255.0f);
	return (uint16_t)(((red & 0xf8U) << 8) | ((green & 0xfcU) << 3) |
			  (blue >> 3));
}
static uint32_t hash32(uint32_t value)
{
	value ^= value >> 16;
	value *= UINT32_C(0x7feb352d);
	value ^= value >> 15;
	value *= UINT32_C(0x846ca68b);
	return value ^ (value >> 16);
}

static int floor_to_int(float value)
{
	int result = (int)value;

	return value < (float)result ? result - 1 : result;
}

static float noise_corner(int x, int y)
{
	uint32_t hash = hash32((uint32_t)x * UINT32_C(0x68bc21eb) ^
			       (uint32_t)y * UINT32_C(0x02e5be93));

	return (float)(hash & UINT32_C(0xffff)) / 65535.0f;
}

static float value_noise(float x, float y)
{
	int cell_x = floor_to_int(x);
	int cell_y = floor_to_int(y);
	float fraction_x = smoothstep(x - (float)cell_x);
	float fraction_y = smoothstep(y - (float)cell_y);
	float lower = noise_corner(cell_x, cell_y) * (1.0f - fraction_x) +
		      noise_corner(cell_x + 1, cell_y) * fraction_x;
	float upper = noise_corner(cell_x, cell_y + 1) * (1.0f - fraction_x) +
		      noise_corner(cell_x + 1, cell_y + 1) * fraction_x;

	return lower * (1.0f - fraction_y) + upper * fraction_y;
}

static float cloud_field(const struct armada_scene *scene, float x, float y)
{
	/* Both octaves are one wind field: toward -x and -z in world space. */
	float drift = (float)scene->frame * 0.0019f;
	float wind_x = x + drift;
	float wind_z = y + drift * 0.48f;
	float broad = value_noise(wind_x, wind_z);
	float detail =
		value_noise(wind_x * 2.03f + 7.3f, wind_z * 2.07f - 3.1f);

	return broad * 0.72f + detail * 0.28f;
}

static float cloud_plane_coverage(const struct armada_scene *scene, float x,
				  float z)
{
	float density = smoothstep(
		(cloud_field(scene, x * 0.22f, z * 0.16f) - 0.54f) / 0.20f);

	return f_clamp(density * 0.68f, 0.0f, 0.68f);
}

static float cloud_coverage_ray(const struct armada_scene *scene,
				struct armada_vec3 origin,
				struct armada_vec3 direction)
{
	float travel =
		(ARMADA_CLOUD_HEIGHT - origin.y) / f_max(direction.y, 0.12f);
	struct armada_vec3 projected =
		vec_add(origin, vec_scale(direction, travel));
	float altitude_envelope =
		smoothstep(f_clamp((direction.y - 0.015f) / 0.17f, 0.0f, 1.0f));

	return cloud_plane_coverage(scene, projected.x, projected.z) *
	       altitude_envelope;
}

static float surface_cloud_coverage(const struct armada_scene *scene,
				    struct armada_vec3 point)
{
	float light_height = scene->celestial_direction.y;
	float projection_weight =
		smoothstep(f_clamp((light_height - 0.04f) / 0.20f, 0.0f, 1.0f));
	float travel =
		(ARMADA_CLOUD_HEIGHT - point.y) / f_max(light_height, 0.12f);
	struct armada_vec3 projected =
		vec_add(point, vec_scale(scene->celestial_direction, travel));
	float projected_coverage =
		cloud_plane_coverage(scene, projected.x, projected.z);
	float global_coverage = f_clamp(
		(1.0f - scene->cloud_transmittance) / 0.20f, 0.0f, 0.68f);

	return global_coverage * (1.0f - projection_weight) +
	       projected_coverage * projection_weight;
}
static float wave_sample(int phase)
{
	static const float samples[64] = {
		0x0p+0f,	 0x1.918324p-4f,  0x1.8f932p-3f,
		0x1.294252p-2f,	 0x1.87db1p-2f,	  0x1.e2b3c6p-2f,
		0x1.1c7238p-1f,	 0x1.44ce8ap-1f,  0x1.6a0ad4p-1f,
		0x1.8bc718p-1f,	 0x1.a9b754p-1f,  0x1.c38b88p-1f,
		0x1.d907b2p-1f,	 0x1.e9f3d4p-1f,  0x1.f627ecp-1f,
		0x1.fd87fcp-1f,	 0x1p+0f,	  0x1.fd87fcp-1f,
		0x1.f627ecp-1f,	 0x1.e9f3d4p-1f,  0x1.d907b2p-1f,
		0x1.c38b88p-1f,	 0x1.a9b754p-1f,  0x1.8bc718p-1f,
		0x1.6a0ad4p-1f,	 0x1.44ce8ap-1f,  0x1.1c7238p-1f,
		0x1.e2b3c6p-2f,	 0x1.87db1p-2f,	  0x1.294252p-2f,
		0x1.8f932p-3f,	 0x1.918324p-4f,  0x0p+0f,
		-0x1.918324p-4f, -0x1.8f932p-3f,  -0x1.294252p-2f,
		-0x1.87db1p-2f,	 -0x1.e2b3c6p-2f, -0x1.1c7238p-1f,
		-0x1.44ce8ap-1f, -0x1.6a0ad4p-1f, -0x1.8bc718p-1f,
		-0x1.a9b754p-1f, -0x1.c38b88p-1f, -0x1.d907b2p-1f,
		-0x1.e9f3d4p-1f, -0x1.f627ecp-1f, -0x1.fd87fcp-1f,
		-0x1p+0f,	 -0x1.fd87fcp-1f, -0x1.f627ecp-1f,
		-0x1.e9f3d4p-1f, -0x1.d907b2p-1f, -0x1.c38b88p-1f,
		-0x1.a9b754p-1f, -0x1.8bc718p-1f, -0x1.6a0ad4p-1f,
		-0x1.44ce8ap-1f, -0x1.1c7238p-1f, -0x1.e2b3c6p-2f,
		-0x1.87db1p-2f,	 -0x1.294252p-2f, -0x1.8f932p-3f,
		-0x1.918324p-4f,
	};

	return samples[(unsigned int)phase & 63U];
}

static float wave_sample_f(float phase)
{
	int first = (int)phase;
	float fraction;

	if (phase < (float)first)
		--first;
	fraction = phase - (float)first;
	return wave_sample(first) * (1.0f - fraction) +
	       wave_sample(first + 1) * fraction;
}
static void prepare_stroke_geometry(struct armada_scene *scene)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(strokes); ++index) {
		const struct armada_stroke *stroke = &strokes[index];
		struct armada_prepared_stroke *prepared =
			&scene->prepared_strokes[index];
		float ax = (float)stroke->ax / 100.0f;
		float ay = (float)stroke->ay / 100.0f;
		float bx = (float)stroke->bx / 100.0f;
		float by = (float)stroke->by / 100.0f;
		float dx = bx - ax;
		float dy = by - ay;
		float inverse = fast_inverse_sqrt(dx * dx + dy * dy);

		prepared->unit_x = dx * inverse;
		prepared->unit_y = dy * inverse;
		prepared->midpoint_x = (ax + bx) * 0.5f;
		prepared->midpoint_y = (ay + by) * 0.5f;
		prepared->half_length = vec_length(vec3(dx, dy, 0.0f)) * 0.5f;
	}
}

static float cycle_solar_phase(uint32_t frame)
{
	return 16.0f - (float)frame * 64.0f / (float)ARMADA_DURATION_FRAMES;
}

static float cycle_daylight(float solar_altitude)
{
	return smoothstep(
		f_clamp((solar_altitude + 0.10f) / 0.72f, 0.0f, 1.0f));
}

static int cycle_lcd_level(uint32_t frame)
{
	float daylight =
		cycle_daylight(wave_sample_f(cycle_solar_phase(frame)));

	return 6 + (int)(daylight * 4.0f + 0.5f);
}

static void update_environment(struct armada_scene *scene)
{
	float solar_phase = cycle_solar_phase(scene->frame);
	float solar_altitude = wave_sample_f(solar_phase);
	float lunar_altitude = -solar_altitude - 0.18f;
	float solar_horizontal = wave_sample_f(solar_phase + 16.0f);
	float sun_weight = smoothstep(
		f_clamp((solar_altitude + 0.05f) / 0.25f, 0.0f, 1.0f));
	float moon_weight = smoothstep(
		f_clamp((lunar_altitude + 0.05f) / 0.25f, 0.0f, 1.0f));
	float moon_mix = smoothstep(
		f_clamp((0.05f - solar_altitude) / 0.28f, 0.0f, 1.0f));

	scene->sun_direction = vec_normalize(
		vec3(solar_horizontal * 0.58f, solar_altitude * 0.82f, 1.0f));
	scene->moon_direction = vec_normalize(
		vec3(-solar_horizontal * 0.58f, lunar_altitude * 0.82f, 1.0f));
	scene->sun_visibility = sun_weight;
	scene->moon_visibility = moon_weight;
	scene->daylight = cycle_daylight(solar_altitude);
	scene->night = smoothstep(
		f_clamp((lunar_altitude + 0.08f) / 0.64f, 0.0f, 1.0f));
	scene->celestial_direction = vec_normalize(color_mix(
		scene->sun_direction, scene->moon_direction, moon_mix));
	scene->light_color = color_mix(vec3(0.72f, 0.88f, 1.0f),
				       vec3(0.46f, 0.68f, 1.0f), moon_mix);
	scene->exposure = 0.78f + scene->daylight * 0.22f;
	scene->cloud_transmittance =
		1.0f - cloud_coverage_ray(scene, vec3(0.0f, 0.75f, -1.35f),
					  scene->celestial_direction) *
			       0.20f;
}

static struct armada_vec3 camera_position(void);

static float motion_progress(float time, float first, float last)
{
	float value = f_clamp((time - first) / (last - first), 0.0f, 1.0f);

	return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

static void letter_targets(float frame,
			   struct armada_letter_state letters[ARMADA_LETTERS])
{
	float time = (float)frame / (float)ARMADA_FRAMES_PER_SECOND;
	float curl = motion_progress(time, 5.5f, 13.0f) *
		     (1.0f - motion_progress(time, 18.0f, 24.0f));
	float turn = 128.0f * motion_progress(time, 6.0f, 23.0f);
	float curvature = curl * 64.0f / (float)ARMADA_LETTERS;
	float spacing = ARMADA_WORD_STEP + (2.15f - ARMADA_WORD_STEP) * curl;
	float plane_yaw = wave_sample_f(turn * 0.5f + 4.0f) * curl * 2.8f;
	float plane_sine = wave_sample_f(plane_yaw);
	float plane_cosine = wave_sample_f(plane_yaw + 16.0f);
	float center_y = ARMADA_WORD_Y + 0.42f + curl * 2.0f +
			 wave_sample_f(curl * 32.0f) * 0.25f;
	float center_z = 4.98f + curl * 0.32f;
	float individuality = 1.0f - curl * 0.35f;
	float path_x[ARMADA_LETTERS] = { 0 };
	float path_y[ARMADA_LETTERS] = { 0 };
	float mean_x = 0.0f;
	float mean_y = 0.0f;
	float projected_sum = 0.0f;
	float inverse_depth_sum = 0.0f;
	float center_x;
	struct armada_vec3 camera = camera_position();
	unsigned int letter;

	/* Equal-length links curl the row into a ring without exchanging letters. */
	for (letter = 1; letter < ARMADA_LETTERS; ++letter) {
		float tangent = turn + ((float)letter - 3.5f) * curvature;

		path_x[letter] = path_x[letter - 1U] +
				 spacing * wave_sample_f(tangent + 16.0f);
		path_y[letter] =
			path_y[letter - 1U] + spacing * wave_sample_f(tangent);
		mean_x += path_x[letter];
		mean_y += path_y[letter];
	}
	mean_x /= (float)ARMADA_LETTERS;
	mean_y /= (float)ARMADA_LETTERS;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		struct armada_letter_state *state = &letters[letter];
		uint32_t delay = letter * ARMADA_LETTER_STAGGER_FRAMES;
		uint32_t sink_start = ARMADA_SINK_FRAME + delay;
		float rise = motion_progress(
			(float)frame, (float)delay,
			(float)(delay + ARMADA_RISE_DURATION_FRAMES));
		float sink = motion_progress(
			(float)frame, (float)sink_start,
			(float)(sink_start + ARMADA_SINK_DURATION_FRAMES));
		float afloat = rise * (1.0f - sink);
		float floating = afloat * individuality;
		float hover_phase = time * (6.4f + (float)letter * 0.31f) +
				    (float)letter * 10.7f;
		float depth_phase = time * (4.6f + (float)letter * 0.23f) +
				    (float)letter * 17.3f;
		float swell = wave_sample_f(hover_phase + 13.0f);
		float hover_y = wave_sample_f(hover_phase) *
				(0.090f + (float)(letter % 3U) * 0.012f);
		float hover_x = wave_sample_f(depth_phase + 16.0f) * 0.018f;
		float hover_z = wave_sample_f(depth_phase) *
				(0.065f + (float)(letter % 2U) * 0.012f);
		float tangent = turn + ((float)letter - 3.0f) * curvature;
		float x = path_x[letter] - mean_x;
		float y = path_y[letter] - mean_y;
		float z = y * 0.20f * curl;
		float nominal_x = x * plane_cosine + z * plane_sine;
		float nominal_z = center_z - x * plane_sine + z * plane_cosine;
		float inverse_depth = 1.0f / (nominal_z - camera.z);

		*state = (struct armada_letter_state){ 0 };
		state->position.x = nominal_x + hover_x * floating;
		state->position.y =
			-1.72f * (1.0f - afloat) +
			(center_y + y * 0.83f + hover_y * individuality) *
				afloat;
		state->position.z =
			5.28f * (1.0f - afloat) +
			(nominal_z + hover_z * individuality) * afloat;
		state->scale = ARMADA_LETTER_SCALE;
		state->scale_x =
			1.06f - afloat * 0.06f - swell * floating * 0.015f;
		state->scale_y =
			0.76f + afloat * 0.24f + swell * floating * 0.028f;
		state->roll = tangent +
			      wave_sample_f(hover_phase + 21.0f) *
				      (0.55f + (float)(letter % 2U) * 0.07f) *
				      floating;
		state->yaw =
			plane_yaw * wave_sample_f(tangent + 16.0f) +
			wave_sample_f(depth_phase + 7.0f) * 0.90f * floating;
		state->pitch =
			-plane_yaw * wave_sample_f(tangent) + curl * 1.80f +
			wave_sample_f(hover_phase + 37.0f) * 0.65f * floating;
		state->visible = frame < (float)ARMADA_SUBMERGED_FRAME;
		projected_sum += nominal_x * inverse_depth;
		inverse_depth_sum += inverse_depth;
	}
	/* Center the formation, without coupling the independent hover offsets. */
	center_x = -projected_sum / inverse_depth_sum;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		letters[letter].position.x += center_x;
}

static void spring_step(float *position, float *velocity, float target,
			float frequency, float damping)
{
	const float step = 1.0f / (float)(ARMADA_FRAMES_PER_SECOND *
					  ARMADA_MOTION_STEPS_PER_FRAME);
	float acceleration = frequency * frequency * (target - *position) -
			     2.0f * damping * frequency * *velocity;

	*velocity += acceleration * step;
	*position += *velocity * step;
}

static void step_letter(struct armada_letter_motion *motion,
			const struct armada_letter_state *target,
			unsigned int letter, bool signal, uint32_t tick)
{
	struct armada_letter_state *pose = &motion->pose;
	struct armada_vec3 previous = pose->position;
	struct armada_water_crossing *crossing = NULL;
	float frequency = 6.0f + (float)letter * 0.10f;
	float stretch;

	spring_step(&pose->position.x, &motion->velocity.x, target->position.x,
		    frequency, 0.48f);
	spring_step(&pose->position.y, &motion->velocity.y, target->position.y,
		    frequency, 0.48f);
	spring_step(&pose->position.z, &motion->velocity.z, target->position.z,
		    frequency, 0.48f);
	/* Lean into the actual movement; angular momentum outlives each turn. */
	spring_step(&pose->yaw, &motion->angular_velocity.x,
		    target->yaw + motion->velocity.z * 1.5f, 8.0f, 0.56f);
	spring_step(&pose->pitch, &motion->angular_velocity.y,
		    target->pitch - motion->velocity.y * 1.4f, 8.0f, 0.56f);
	/* Angles stay unwrapped here: two complete turns are 128, not zero. */
	spring_step(&pose->roll, &motion->angular_velocity.z,
		    target->roll - motion->velocity.x * 0.8f, 8.0f, 0.56f);
	stretch = f_clamp(motion->velocity.y * 0.025f, -0.10f, 0.10f);
	spring_step(&pose->scale_x, &motion->stretch_velocity_x,
		    target->scale_x - stretch * 0.5f, 12.0f, 0.60f);
	spring_step(&pose->scale_y, &motion->stretch_velocity_y,
		    target->scale_y + stretch, 12.0f, 0.60f);
	spring_step(&motion->recoil, &motion->recoil_velocity,
		    signal ? 1.0f : 0.0f, 24.0f, 0.60f);
	/* End the invisible recoil tail before it reaches subnormal floats. */
	if (!signal && f_abs(motion->recoil) < 0.00001f &&
	    f_abs(motion->recoil_velocity) < 0.0001f) {
		motion->recoil = 0.0f;
		motion->recoil_velocity = 0.0f;
	}
	pose->visible = target->visible;

	if (!motion->rise.tick && previous.y < ARMADA_WATER_Y &&
	    pose->position.y >= ARMADA_WATER_Y)
		crossing = &motion->rise;
	else if (!motion->sink.tick &&
		 tick >= ARMADA_SINK_FRAME * ARMADA_MOTION_STEPS_PER_FRAME &&
		 previous.y > ARMADA_WATER_Y &&
		 pose->position.y <= ARMADA_WATER_Y)
		crossing = &motion->sink;
	if (crossing) {
		float fraction = (ARMADA_WATER_Y - previous.y) /
				 (pose->position.y - previous.y);

		crossing->position = vec_add(
			previous,
			vec_scale(vec_sub(pose->position, previous), fraction));
		crossing->tick = tick;
	}
}

static void advance_letters(struct armada_scene *scene)
{
	uint32_t target_tick = scene->frame * ARMADA_MOTION_STEPS_PER_FRAME;
	struct armada_letter_state targets[ARMADA_LETTERS];
	unsigned int letter;

	/* Fixed steps make seeks, skipped frames and normal playback identical. */
	if (!scene->motion_initialized || target_tick < scene->motion_tick) {
		memset(scene->motion, 0, sizeof(scene->motion));
		letter_targets(0.0f, targets);
		for (letter = 0; letter < ARMADA_LETTERS; ++letter)
			scene->motion[letter].pose = targets[letter];
		scene->motion_tick = 0U;
		scene->motion_initialized = true;
	}
	while (scene->motion_tick < target_tick) {
		struct armada_morse_symbol symbol;
		bool signal;

		++scene->motion_tick;
		letter_targets((float)scene->motion_tick /
				       (float)ARMADA_MOTION_STEPS_PER_FRAME,
			       targets);
		signal = morse_symbol_for_frame(
			scene->motion_tick / ARMADA_MOTION_STEPS_PER_FRAME,
			&symbol);
		for (letter = 0; letter < ARMADA_LETTERS; ++letter)
			step_letter(&scene->motion[letter], &targets[letter],
				    letter, signal && symbol.letter == letter,
				    scene->motion_tick);
	}
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		scene->letters[letter] = scene->motion[letter].pose;
}

static void prepare_ripples(struct armada_scene *scene)
{
	bool sinking = scene->frame >= ARMADA_SINK_FRAME;
	unsigned int letter;

	scene->ripple_count = 0U;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_water_crossing *event =
			sinking ? &scene->motion[letter].sink :
				  &scene->motion[letter].rise;
		float emitted_frame = (float)event->tick /
				      (float)ARMADA_MOTION_STEPS_PER_FRAME;
		float age = (float)scene->frame - emitted_frame;
		float lifetime = sinking ? (float)ARMADA_DURATION_FRAMES -
						   emitted_frame :
					   (float)ARMADA_RISE_RIPPLE_DURATION;
		struct armada_ripple *ripple;
		float attack;
		float decay;

		if (!event->tick || age < 0.0f || age >= lifetime)
			continue;
		ripple = &scene->ripples[scene->ripple_count++];
		attack = smoothstep(f_clamp(age / 6.0f, 0.0f, 1.0f));
		decay = smoothstep(
			f_clamp((lifetime - age) / (sinking ? 86.0f : 64.0f),
				0.0f, 1.0f));
		ripple->origin = event->position;
		ripple->radius = (sinking ? 0.026f : 0.023f) * age;
		ripple->amplitude =
			(sinking ? 0.050f : 0.032f) * attack * decay;
	}
}

static void apply_impact(struct armada_scene *scene, unsigned int letter)
{
	struct armada_morse_symbol symbol;
	float attack;
	float release;
	float envelope;
	float recoil = scene->motion[letter].recoil;

	/* Apply to the rendered copy, never to the integrated physical pose. */
	scene->letters[letter].position.z -= recoil * 0.42f;
	scene->letters[letter].scale *= 1.0f + recoil * 0.13f;
	scene->letters[letter].pitch += recoil * 0.28f;

	if (!morse_symbol_for_frame(scene->frame, &symbol) ||
	    symbol.letter != letter)
		return;
	if (symbol.duration == ARMADA_MORSE_UNIT_FRAMES) {
		attack = smoothstep((float)(symbol.elapsed + 1U) / 1.5f);
		release = smoothstep((float)(symbol.duration - symbol.elapsed) /
				     1.5f);
	} else {
		attack = smoothstep((float)(symbol.elapsed + 1U) / 2.0f);
		release = smoothstep((float)(symbol.duration - symbol.elapsed) /
				     2.0f);
	}
	envelope = attack * release;
	scene->letters[letter].emissive =
		f_max(scene->letters[letter].emissive, envelope);
}

static void prepare_rotations(struct armada_scene *scene)
{
	unsigned int index;

	for (index = 0; index < ARMADA_LETTERS; ++index) {
		struct armada_letter_state *letter = &scene->letters[index];

		letter->yaw_sine = wave_sample_f(letter->yaw);
		letter->yaw_cosine = wave_sample_f(letter->yaw + 16.0f);
		letter->pitch_sine = wave_sample_f(letter->pitch);
		letter->pitch_cosine = wave_sample_f(letter->pitch + 16.0f);
		letter->roll_sine = wave_sample_f(letter->roll);
		letter->roll_cosine = wave_sample_f(letter->roll + 16.0f);
	}
}

static struct armada_vec3
rotate_forward(const struct armada_letter_state *letter,
	       struct armada_vec3 value)
{
	float yaw_x = letter->yaw_cosine * value.x + letter->yaw_sine * value.z;
	float yaw_z =
		-letter->yaw_sine * value.x + letter->yaw_cosine * value.z;
	float pitch_y =
		letter->pitch_cosine * value.y - letter->pitch_sine * yaw_z;
	float pitch_z =
		letter->pitch_sine * value.y + letter->pitch_cosine * yaw_z;

	return vec3(letter->roll_cosine * yaw_x - letter->roll_sine * pitch_y,
		    letter->roll_sine * yaw_x + letter->roll_cosine * pitch_y,
		    pitch_z);
}

static void update_storyboard(struct armada_scene *scene)
{
	unsigned int letter;

	advance_letters(scene);
	prepare_ripples(scene);
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		apply_impact(scene, letter);
	update_environment(scene);
	prepare_rotations(scene);
}

static struct armada_vec3 star_color(const struct armada_scene *scene,
				     struct armada_vec3 direction)
{
	float projection = f_max(direction.z, 0.05f);
	int cell_x;
	int cell_y;
	uint32_t hash;
	unsigned int twinkle;
	float visibility = 0.06f + scene->night * 0.94f;

	if (direction.y <= 0.0f)
		return vec3(0.0f, 0.0f, 0.0f);
	/* Sky and rough-water reflection address this same projected star field. */
	cell_x = floor_to_int((direction.x / projection + 1.8f) * 96.0f);
	cell_y = floor_to_int((direction.y / projection + 0.2f) * 96.0f);
	hash = hash32((uint32_t)cell_x * UINT32_C(73856093) ^
		      (uint32_t)cell_y * UINT32_C(19349663));
	twinkle = (unsigned int)((scene->frame / 18U + (hash >> 11)) & 3U);
	if ((hash & 511U) >= 3U)
		return vec3(0.0f, 0.0f, 0.0f);
	return vec_scale(vec3(0.62f, 0.82f, 1.0f),
			 visibility * (0.30f + (float)twinkle * 0.14f));
}

static float celestial_plane_distance_squared(struct armada_vec3 direction,
					      struct armada_vec3 body)
{
	float direction_z = f_max(direction.z, 0.05f);
	float body_z = f_max(body.z, 0.05f);
	float dx = direction.x / direction_z - body.x / body_z;
	float dy = direction.y / direction_z - body.y / body_z;

	return dx * dx + dy * dy;
}

static float celestial_body_edge(const struct armada_scene *scene,
				 struct armada_vec3 direction,
				 struct armada_vec3 body, float radius)
{
	float pixel = 2.0f / ((float)scene->height * 0.95f);
	float inner = f_max(radius - pixel, radius * 0.55f);
	float radius_squared = radius * radius;
	float inner_squared = inner * inner;
	float distance_squared =
		celestial_plane_distance_squared(direction, body);

	return smoothstep((radius_squared - distance_squared) /
			  (radius_squared - inner_squared));
}

static struct armada_vec3 sun_body_color(const struct armada_scene *scene)
{
	float high_sun =
		smoothstep(f_clamp(scene->sun_direction.y / 0.42f, 0.0f, 1.0f));

	return color_mix(vec3(1.0f, 0.34f, 0.08f), vec3(1.0f, 0.97f, 0.76f),
			 high_sun);
}

static struct armada_vec3 sky_color(const struct armada_scene *scene,
				    struct armada_vec3 origin,
				    struct armada_vec3 direction,
				    struct armada_vec3 light_transmission)
{
	float height = f_clamp(direction.y * 1.55f + 0.30f, 0.0f, 1.0f);
	struct armada_vec3 day = color_mix(vec3(0.24f, 0.62f, 0.82f),
					   vec3(0.035f, 0.26f, 0.68f), height);
	struct armada_vec3 night = color_mix(vec3(0.018f, 0.055f, 0.13f),
					     vec3(0.002f, 0.008f, 0.038f),
					     height);
	struct armada_vec3 color =
		color_mix(night, day, f_clamp(scene->daylight, 0.0f, 1.0f));
	float twilight = f_clamp(1.0f - scene->daylight - scene->night * 0.35f,
				 0.0f, 1.0f);
	float sun_body = vec_dot(direction, scene->sun_direction);
	float sun_edge = celestial_body_edge(
		scene, direction, scene->sun_direction, ARMADA_SUN_RADIUS);
	float moon_edge = celestial_body_edge(
		scene, direction, scene->moon_direction, ARMADA_MOON_RADIUS);
	float coverage = cloud_coverage_ray(scene, origin, direction);
	float cloud_opacity = coverage * 0.68f;

	if (twilight > 0.0f)
		color = color_mix(color, vec3(0.44f, 0.105f, 0.065f),
				  twilight * (1.0f - height) * 0.72f);
	if (sun_edge > 0.0f) {
		color = vec_add(
			color,
			color_filter(
				vec_scale(vec_sub(sun_body_color(scene), color),
					  sun_edge * scene->sun_visibility),
				light_transmission));
	}
	if (moon_edge > 0.0f)
		color = vec_add(
			color,
			color_filter(
				vec_scale(vec_sub(vec3(0.68f, 0.80f, 1.0f),
						  color),
					  moon_edge * scene->moon_visibility),
				light_transmission));
	color = vec_add(color, star_color(scene, direction));
	{
		float light_facing = smoothstep(f_clamp(
			(vec_dot(direction, scene->celestial_direction) -
			 0.72f) /
				0.28f,
			0.0f, 1.0f));
		float warm_edge = twilight *
				  smoothstep(f_clamp((sun_body - 0.90f) / 0.10f,
						     0.0f, 1.0f));
		struct armada_vec3 day_cloud =
			color_mix(vec3(0.54f, 0.64f, 0.72f),
				  vec3(0.92f, 0.95f, 0.97f), light_facing);
		struct armada_vec3 night_cloud = color_mix(
			vec3(0.060f, 0.100f, 0.180f), vec3(0.30f, 0.38f, 0.55f),
			light_facing * 0.80f);
		struct armada_vec3 cloud_color =
			color_mix(night_cloud, day_cloud, scene->daylight);

		cloud_color = color_mix(cloud_color, vec3(0.90f, 0.34f, 0.16f),
					warm_edge * 0.46f);
		color = color_mix(color, cloud_color, cloud_opacity);
	}
	return color;
}

static struct armada_vec3 letter_base_color(unsigned int letter)
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

static void add_ripple_ring(struct armada_water_sample *sample, float x,
			    float z, float radius, float amplitude)
{
	const float width = 0.30f;
	float radius_squared = radius * radius;
	float band = 2.0f * radius * width + width * width;
	float q = (x * x + z * z - radius_squared) / band;
	float u;
	float profile;
	float derivative;

	if (f_abs(q) >= 1.0f)
		return;
	u = 1.0f - q * q;
	profile = (1.0f - 4.0f * q * q) * u * u;
	derivative = -12.0f * q * u * (1.0f - 2.0f * q * q);
	sample->height += amplitude * profile;
	sample->gradient_x += amplitude * derivative * 2.0f * x / band;
	sample->gradient_z += amplitude * derivative * 2.0f * z / band;
}

static void add_letter_ripples(const struct armada_scene *scene, float x,
			       float z, struct armada_water_sample *sample)
{
	unsigned int index;

	for (index = 0; index < scene->ripple_count; ++index) {
		const struct armada_ripple *ripple = &scene->ripples[index];

		add_ripple_ring(sample, x - ripple->origin.x,
				z - ripple->origin.z, ripple->radius,
				ripple->amplitude);
	}
}

static struct armada_water_sample water_sample(const struct armada_scene *scene,
					       float x, float z)
{
	const float radians_per_phase = 0.09817477f;
	float phase_one = x * 6.0f + z * 3.0f + (float)scene->frame * 0.36f;
	float phase_two = x * -4.0f + z * 8.0f - (float)scene->frame * 0.29f;
	float phase_three = x * 10.0f - z * 5.0f + (float)scene->frame * 0.48f;
	float cosine_one = wave_sample_f(phase_one + 16.0f);
	float cosine_two = wave_sample_f(phase_two + 16.0f);
	float cosine_three = wave_sample_f(phase_three + 16.0f);
	struct armada_water_sample sample = {
		.height = ARMADA_WATER_Y + wave_sample_f(phase_one) * 0.035f +
			  wave_sample_f(phase_two) * 0.026f +
			  wave_sample_f(phase_three) * 0.019f,
		.gradient_x = cosine_one * 0.035f * radians_per_phase * 6.0f +
			      cosine_two * 0.026f * radians_per_phase * -4.0f +
			      cosine_three * 0.019f * radians_per_phase * 10.0f,
		.gradient_z = cosine_one * 0.035f * radians_per_phase * 3.0f +
			      cosine_two * 0.026f * radians_per_phase * 8.0f +
			      cosine_three * 0.019f * radians_per_phase * -5.0f,
	};

	add_letter_ripples(scene, x, z, &sample);
	return sample;
}

static bool water_intersection(const struct armada_scene *scene,
			       struct armada_vec3 origin,
			       struct armada_vec3 direction, float *distance)
{
	float travel;
	unsigned int iteration;

	if (direction.y >= -0.0001f)
		return false;
	travel = (ARMADA_WATER_Y - origin.y) / direction.y;
	for (iteration = 0; iteration < 2U; ++iteration) {
		struct armada_vec3 current =
			vec_add(origin, vec_scale(direction, travel));
		struct armada_water_sample current_sample =
			water_sample(scene, current.x, current.z);
		float derivative = direction.y -
				   current_sample.gradient_x * direction.x -
				   current_sample.gradient_z * direction.z;
		float residual = current.y - current_sample.height;

		if (f_abs(derivative) < 0.025f)
			derivative = derivative < 0.0f ? -0.025f : 0.025f;
		travel -= residual / derivative;
	}
	if (travel <= 0.0f || travel >= ARMADA_WATER_FAR_DISTANCE)
		return false;
	*distance = travel;
	return true;
}

static struct armada_vec3 water_normal(const struct armada_scene *scene,
				       struct armada_vec3 point,
				       struct armada_water_sample sample,
				       float distance)
{
	float distance_fade =
		1.0f - smoothstep(f_clamp(
			       (distance - ARMADA_NEAR_WATER_DISTANCE) / 34.0f,
			       0.0f, 1.0f)) *
			       0.88f;
	float ripple_fade =
		1.0f - smoothstep(f_clamp(
			       (distance - ARMADA_NEAR_WATER_DISTANCE) / 19.0f,
			       0.0f, 1.0f)) *
			       0.96f;
	float ripple_one = wave_sample_f(point.x * 18.0f + point.z * 11.0f +
					 (float)scene->frame * 0.76f + 16.0f);
	float ripple_two = wave_sample_f(point.x * -15.0f + point.z * 21.0f -
					 (float)scene->frame * 0.63f + 16.0f);
	float gradient_x =
		sample.gradient_x * distance_fade +
		(ripple_one * 0.032f - ripple_two * 0.021f) * ripple_fade;
	float gradient_z =
		sample.gradient_z * distance_fade +
		(ripple_one * 0.020f + ripple_two * 0.030f) * ripple_fade;

	return vec_normalize(vec3(-gradient_x, 1.0f, -gradient_z));
}

static struct armada_vec3 water_body_glint(struct armada_vec3 reflected,
					   struct armada_vec3 body_direction,
					   float visibility,
					   struct armada_vec3 body_color,
					   float cloud_transmittance,
					   float intensity)
{
	float reflected_z;
	float body_z;
	float horizontal_distance;
	float vertical_distance;
	float across;
	float along;

	if (visibility <= 0.0f || body_direction.y <= 0.0f)
		return vec3(0.0f, 0.0f, 0.0f);
	reflected_z = f_max(reflected.z, 0.05f);
	body_z = f_max(body_direction.z, 0.05f);
	horizontal_distance =
		f_abs(reflected.x / reflected_z - body_direction.x / body_z);
	vertical_distance =
		f_abs(reflected.y / reflected_z - body_direction.y / body_z);
	across = smoothstep(1.0f - horizontal_distance / 0.075f);
	along = smoothstep(1.0f - vertical_distance / 0.38f);

	/* Directional waves stretch a distant reflection along the view axis. */
	return vec_scale(body_color, visibility * cloud_transmittance * across *
					     across * along * intensity);
}

static struct armada_vec3
triangle_background_water(const struct armada_scene *scene,
			  struct armada_vec3 point,
			  struct armada_vec3 direction, float distance,
			  struct armada_water_sample sample,
			  struct armada_vec3 light_transmission)
{
	struct armada_vec3 normal =
		water_normal(scene, point, sample, distance);
	struct armada_vec3 reflected = vec_reflect(direction, normal);
	struct armada_vec3 view = vec_scale(direction, -1.0f);
	float facing = f_clamp(vec_dot(normal, view), 0.0f, 1.0f);
	float grazing = 1.0f - facing;
	float fresnel = 0.040f + 0.960f * grazing * grazing * grazing *
					 grazing * grazing;
	float absorption =
		f_clamp(distance / ARMADA_WATER_FAR_DISTANCE, 0.0f, 1.0f);
	struct armada_vec3 near_water = color_mix(vec3(0.012f, 0.125f, 0.16f),
						  vec3(0.025f, 0.30f, 0.36f),
						  scene->daylight);
	struct armada_vec3 deep_water = color_mix(vec3(0.002f, 0.022f, 0.060f),
						  vec3(0.005f, 0.09f, 0.18f),
						  scene->daylight);
	struct armada_vec3 base =
		color_mix(near_water, deep_water, absorption * 0.82f);
	float crest = smoothstep(
		f_clamp((sample.height - ARMADA_WATER_Y + 0.008f) / 0.070f,
			0.0f, 1.0f));
	float steepness = smoothstep(f_clamp(
		(f_abs(sample.gradient_x) + f_abs(sample.gradient_z) - 0.018f) /
			0.075f,
		0.0f, 1.0f));
	struct armada_vec3 crest_color = color_mix(vec3(0.025f, 0.20f, 0.29f),
						   vec3(0.16f, 0.58f, 0.62f),
						   scene->daylight);
	float surface_cloud = surface_cloud_coverage(scene, point);
	struct armada_vec3 reflection =
		sky_color(scene, point, reflected, light_transmission);
	struct armada_vec3 glint = vec_add(
		water_body_glint(reflected, scene->sun_direction,
				 scene->sun_visibility, sun_body_color(scene),
				 1.0f - surface_cloud, 0.30f),
		water_body_glint(reflected, scene->moon_direction,
				 scene->moon_visibility,
				 vec3(0.68f, 0.80f, 1.0f), 1.0f - surface_cloud,
				 0.16f));

	base = color_mix(base, crest_color,
			 crest * steepness * (0.07f + scene->daylight * 0.07f));
	base = vec_scale(base, 1.0f - surface_cloud * 0.16f);
	/* Filter direct illumination, not the ambient sky reflected by the water. */
	base = color_filter(base, color_mix(vec3(1.0f, 1.0f, 1.0f),
					    light_transmission, 0.65f));
	glint = color_filter(glint, light_transmission);
	return vec_scale(vec_add(color_mix(base, reflection, fresnel), glint),
			 scene->exposure);
}

static struct armada_vec3 camera_position(void)
{
	return vec3(0.0f, 0.75f, -1.35f);
}

static struct armada_vec3 output_ray(const struct armada_scene *scene,
				     unsigned int x, unsigned int y)
{
	float denominator = (float)scene->height * 0.95f;

	return vec_normalize(
		vec3(((float)(2U * x + 1U) - (float)scene->width) / denominator,
		     ((float)scene->height * 1.125f - (float)(2U * y + 1U)) /
			     denominator,
		     1.0f));
}

static struct armada_vec3 shade_far_water(const struct armada_scene *scene,
					  struct armada_vec3 direction,
					  float distance,
					  struct armada_water_sample sample,
					  struct armada_vec3 light_transmission)
{
	struct armada_vec3 point =
		vec_add(camera_position(), vec_scale(direction, distance));
	struct armada_vec3 deep = color_mix(vec3(0.002f, 0.022f, 0.060f),
					    vec3(0.005f, 0.09f, 0.18f),
					    scene->daylight);
	float haze = f_clamp((distance - ARMADA_NEAR_WATER_DISTANCE) /
				     (ARMADA_WATER_FAR_DISTANCE -
				      ARMADA_NEAR_WATER_DISTANCE),
			     0.0f, 1.0f);
	struct armada_vec3 normal =
		water_normal(scene, point, sample, distance);
	struct armada_vec3 reflected = vec_reflect(direction, normal);
	struct armada_vec3 view = vec_scale(direction, -1.0f);
	struct armada_vec3 sky =
		sky_color(scene, point, reflected, light_transmission);
	float grazing = 1.0f - f_clamp(vec_dot(normal, view), 0.0f, 1.0f);
	float squared = grazing * grazing;
	float fresnel = 0.040f + 0.960f * squared * squared * grazing;
	struct armada_vec3 base = color_mix(deep, sky, haze * 0.32f);
	float surface_cloud = surface_cloud_coverage(scene, point);
	struct armada_vec3 glint = vec_add(
		water_body_glint(reflected, scene->sun_direction,
				 scene->sun_visibility, sun_body_color(scene),
				 1.0f - surface_cloud, 0.30f),
		water_body_glint(reflected, scene->moon_direction,
				 scene->moon_visibility,
				 vec3(0.68f, 0.80f, 1.0f), 1.0f - surface_cloud,
				 0.16f));

	base = color_filter(base, color_mix(vec3(1.0f, 1.0f, 1.0f),
					    light_transmission, 0.65f));
	glint = color_filter(glint, light_transmission);
	return vec_scale(vec_add(color_mix(base, sky, fresnel), glint),
			 scene->exposure);
}

static struct armada_vec3
water_light_transmission(const struct armada_scene *scene,
			 struct armada_vec3 point);

static struct armada_vec3
triangle_background_sample(const struct armada_scene *scene,
			   struct armada_vec3 direction)
{
	struct armada_vec3 camera = camera_position();
	struct armada_vec3 point;
	struct armada_water_sample water;
	float distance;

	if (water_intersection(scene, camera, direction, &distance)) {
		struct armada_vec3 transmission;

		point = vec_add(camera, vec_scale(direction, distance));
		water = water_sample(scene, point.x, point.z);
		transmission = water_light_transmission(scene, point);
		if (distance > ARMADA_NEAR_WATER_DISTANCE)
			return shade_far_water(scene, direction, distance,
					       water, transmission);
		return triangle_background_water(scene, point, direction,
						 distance, water, transmission);
	}
	return vec_scale(sky_color(scene, camera, direction,
				   vec3(1.0f, 1.0f, 1.0f)),
			 scene->exposure);
}

static void upscale_background(struct armada_scene *scene)
{
	unsigned int source_y;

	for (source_y = 0; source_y < scene->low_height; ++source_y) {
		unsigned int next_y = source_y + 1U < scene->low_height ?
					      source_y + 1U :
					      source_y;
		const uint16_t *row0 =
			scene->low_pixels + (size_t)source_y * scene->low_width;
		const uint16_t *row1 =
			scene->low_pixels + (size_t)next_y * scene->low_width;
		unsigned int fraction_y;

		for (fraction_y = 0; fraction_y < ARMADA_BACKGROUND_SCALE;
		     ++fraction_y) {
			unsigned int y =
				source_y * ARMADA_BACKGROUND_SCALE + fraction_y;
			unsigned int vertical0 =
				ARMADA_BACKGROUND_SCALE - fraction_y;
			unsigned int source_x;

			if (y >= scene->height)
				break;
			for (source_x = 0; source_x < scene->low_width;
			     ++source_x) {
				unsigned int next_x =
					source_x + 1U < scene->low_width ?
						source_x + 1U :
						source_x;
				uint16_t samples[4] = {
					row0[source_x],
					row0[next_x],
					row1[source_x],
					row1[next_x],
				};
				unsigned int left_red =
					((samples[0] >> 11) & 0x1fU) *
						vertical0 +
					((samples[2] >> 11) & 0x1fU) *
						fraction_y;
				unsigned int right_red =
					((samples[1] >> 11) & 0x1fU) *
						vertical0 +
					((samples[3] >> 11) & 0x1fU) *
						fraction_y;
				unsigned int left_green =
					((samples[0] >> 5) & 0x3fU) *
						vertical0 +
					((samples[2] >> 5) & 0x3fU) *
						fraction_y;
				unsigned int right_green =
					((samples[1] >> 5) & 0x3fU) *
						vertical0 +
					((samples[3] >> 5) & 0x3fU) *
						fraction_y;
				unsigned int left_blue =
					(samples[0] & 0x1fU) * vertical0 +
					(samples[2] & 0x1fU) * fraction_y;
				unsigned int right_blue =
					(samples[1] & 0x1fU) * vertical0 +
					(samples[3] & 0x1fU) * fraction_y;
				unsigned int fraction_x;

				for (fraction_x = 0;
				     fraction_x < ARMADA_BACKGROUND_SCALE;
				     ++fraction_x) {
					unsigned int x =
						source_x *
							ARMADA_BACKGROUND_SCALE +
						fraction_x;
					unsigned int horizontal0 =
						ARMADA_BACKGROUND_SCALE -
						fraction_x;
					unsigned int red;
					unsigned int green;
					unsigned int blue;

					if (x >= scene->width)
						break;
					red = left_red * horizontal0 +
					      right_red * fraction_x;
					green = left_green * horizontal0 +
						right_green * fraction_x;
					blue = left_blue * horizontal0 +
					       right_blue * fraction_x;
					scene->pixels[(size_t)y * scene->width +
						      x] =
						(uint16_t)(((red / 16U) << 11) |
							   ((green / 16U)
							    << 5) |
							   (blue / 16U));
				}
			}
		}
	}
}

static void refine_celestial_body(struct armada_scene *scene,
				  struct armada_vec3 body_direction,
				  float visibility, float radius)
{
	float denominator = (float)scene->height * 0.95f;
	/* Four upscaled texels can carry the low-resolution body's bilinear halo. */
	float candidate_radius = radius + 8.0f / denominator;
	float center_x;
	float center_y;
	int extent = (int)(candidate_radius * denominator * 0.5f + 2.0f);
	int minimum_x;
	int maximum_x;
	int minimum_y;
	int maximum_y;
	int y;

	if (visibility <= 0.0f || body_direction.z <= 0.05f)
		return;
	center_x = ((float)scene->width +
		    body_direction.x / body_direction.z * denominator - 1.0f) *
		   0.5f;
	center_y = ((float)scene->height * 1.125f -
		    body_direction.y / body_direction.z * denominator - 1.0f) *
		   0.5f;
	minimum_x = (int)(center_x - (float)extent);
	maximum_x = (int)(center_x + (float)extent);
	minimum_y = (int)(center_y - (float)extent);
	maximum_y = (int)(center_y + (float)extent);
	if (maximum_x < 0 || maximum_y < 0 || minimum_x >= (int)scene->width ||
	    minimum_y >= (int)scene->height)
		return;
	minimum_x = minimum_x < 0 ? 0 : minimum_x;
	minimum_y = minimum_y < 0 ? 0 : minimum_y;
	maximum_x = maximum_x >= (int)scene->width ? (int)scene->width - 1 :
						     maximum_x;
	maximum_y = maximum_y >= (int)scene->height ? (int)scene->height - 1 :
						      maximum_y;

	for (y = minimum_y; y <= maximum_y; ++y) {
		int x;

		for (x = minimum_x; x <= maximum_x; ++x) {
			struct armada_vec3 direction = output_ray(
				scene, (unsigned int)x, (unsigned int)y);
			struct armada_vec3 color;

			if (celestial_plane_distance_squared(direction,
							     body_direction) >
			    candidate_radius * candidate_radius)
				continue;
			color = triangle_background_sample(scene, direction);
			scene->pixels[(size_t)y * scene->width +
				      (unsigned int)x] = color_to_rgb565(color);
		}
	}
}

static void refine_celestial_bodies(struct armada_scene *scene)
{
	refine_celestial_body(scene, scene->sun_direction,
			      scene->sun_visibility, ARMADA_SUN_RADIUS);
	refine_celestial_body(scene, scene->moon_direction,
			      scene->moon_visibility, ARMADA_MOON_RADIUS);
}

struct triangle_vertex {
	struct armada_vec3 world;
	float x;
	float y;
	float inverse_z;
};

struct armada_glass_surface {
	struct armada_vec3 light;
	struct armada_vec3 transmission;
	float offset_x;
	float offset_y;
};

static struct armada_vec3
triangle_stroke_world(const struct armada_scene *scene, unsigned int letter,
		      const struct armada_prepared_stroke *stroke, float along,
		      float across, float depth, bool reflection)
{
	const struct armada_letter_state *state = &scene->letters[letter];
	float glyph_x = stroke->midpoint_x + along * stroke->unit_x -
			across * stroke->unit_y;
	float glyph_y = stroke->midpoint_y + along * stroke->unit_y +
			across * stroke->unit_x;
	float scaled_y = glyph_y * state->scale_y;
	struct armada_vec3 local =
		vec3(glyph_x * state->scale_x, scaled_y, depth);
	struct armada_vec3 world =
		vec_add(state->position,
			vec_scale(rotate_forward(state, local), state->scale));

	if (reflection)
		world.y = 2.0f * ARMADA_WATER_Y - world.y;
	return world;
}

static bool triangle_project(const struct armada_scene *scene,
			     struct triangle_vertex *vertex, bool reflection)
{
	struct armada_vec3 relative = vec_sub(vertex->world, camera_position());
	float scale = (float)scene->height * 0.95f;

	if (relative.z <= 0.05f)
		return false;
	vertex->x =
		((float)scene->width + relative.x / relative.z * scale - 1.0f) *
		0.5f;
	vertex->y = ((float)scene->height * 1.125f -
		     relative.y / relative.z * scale - 1.0f) *
		    0.5f;
	if (reflection) {
		struct armada_water_sample water =
			water_sample(scene, vertex->world.x, vertex->world.z);

		vertex->x += water.gradient_x * 14.0f + water.gradient_z * 3.0f;
		vertex->y += (water.height - ARMADA_WATER_Y) * 9.0f +
			     water.gradient_z * 5.0f;
	}
	vertex->inverse_z = 1.0f / relative.z;
	return true;
}

static struct armada_vec3 triangle_cross(struct armada_vec3 a,
					 struct armada_vec3 b)
{
	return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
		    a.x * b.y - a.y * b.x);
}

static float triangle_edge(const struct triangle_vertex *a,
			   const struct triangle_vertex *b, float x, float y)
{
	return (x - a->x) * (b->y - a->y) - (y - a->y) * (b->x - a->x);
}

static uint16_t triangle_mix_rgb565(uint16_t background,
				    struct armada_vec3 foreground, float alpha)
{
	float inverse = 1.0f - alpha;
	struct armada_vec3 color =
		vec3(((float)((background >> 11) & 0x1fU) / 31.0f) * inverse +
			     foreground.x * alpha,
		     ((float)((background >> 5) & 0x3fU) / 63.0f) * inverse +
			     foreground.y * alpha,
		     ((float)(background & 0x1fU) / 31.0f) * inverse +
			     foreground.z * alpha);

	return color_to_rgb565(color);
}

static struct armada_glass_surface
prepare_glass_surface(const struct armada_scene *scene, unsigned int letter,
		      struct armada_vec3 normal, struct armada_vec3 point,
		      bool reflection)
{
	struct armada_vec3 base = letter_base_color(letter);
	struct armada_vec3 relative = vec_sub(point, camera_position());
	struct armada_vec3 incident = vec_normalize(relative);
	float facing = f_clamp(-vec_dot(normal, incident), 0.0f, 1.0f);
	float eta = 1.0f / ARMADA_GLASS_IOR;
	float transmitted_squared = 1.0f - eta * eta * (1.0f - facing * facing);
	float transmitted_cosine =
		transmitted_squared * fast_inverse_sqrt(transmitted_squared);
	struct armada_vec3 through =
		vec_add(vec_scale(incident, eta),
			vec_scale(normal, eta * facing - transmitted_cosine));
	float thickness = 0.34f * scene->letters[letter].scale;
	struct armada_vec3 exit_point = vec_add(
		relative, vec_scale(through, thickness / transmitted_cosine));
	float focal = (float)scene->height * 0.475f;
	float rim = 1.0f - facing;
	float rim_squared = rim * rim;
	float reflectance = 0.04f + 0.96f * rim_squared * rim_squared * rim;
	struct armada_vec3 view = vec_scale(incident, -1.0f);
	struct armada_vec3 reflected = vec_reflect(incident, normal);
	struct armada_vec3 half_vector;
	struct armada_vec3 environment;
	struct armada_glass_surface glass;
	float diffuse;
	float highlight;
	float absorption = ARMADA_GLASS_ABSORPTION + rim * 0.20f;
	float scatter;

	glass.offset_x =
		focal * (exit_point.x / exit_point.z - relative.x / relative.z);
	glass.offset_y = -focal * (exit_point.y / exit_point.z -
				   relative.y / relative.z);
	/* Mirrored geometry sees the same light through the reflected camera. */
	if (reflection) {
		point.y = 2.0f * ARMADA_WATER_Y - point.y;
		normal.y = -normal.y;
		view.y = -view.y;
		reflected.y = -reflected.y;
	}
	half_vector = vec_normalize(vec_add(scene->celestial_direction, view));
	diffuse = f_max(vec_dot(normal, scene->celestial_direction), 0.0f);
	highlight = f_max(vec_dot(normal, half_vector), 0.0f);
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	environment = reflected.y > 0.0f ?
			      sky_color(scene, point, reflected,
					vec3(1.0f, 1.0f, 1.0f)) :
			      color_mix(vec3(0.008f, 0.045f, 0.075f),
					vec3(0.055f, 0.24f, 0.30f),
					scene->daylight);
	/* A little colored scattering keeps the small stained-glass glyphs readable. */
	scatter = (0.80f + 0.18f * scene->daylight + diffuse * 0.12f) *
		  (1.0f - reflectance);
	glass.light = vec_add(vec_scale(environment, reflectance),
			      vec_scale(base, scatter));
	glass.light = vec_add(
		glass.light,
		vec_scale(base, rim_squared * (0.18f + scene->night * 0.15f)));
	glass.light = vec_add(
		glass.light,
		vec_scale(scene->light_color,
			  highlight * 0.38f * scene->cloud_transmittance));
	glass.light = vec_add(
		glass.light,
		vec_scale(color_mix(base, vec3(1.0f, 1.0f, 1.0f), 0.65f),
			  scene->letters[letter].emissive *
				  (0.26f + rim * 0.55f)));
	glass.light = vec_scale(glass.light, scene->exposure);
	glass.transmission =
		vec_scale(color_mix(vec3(1.0f, 1.0f, 1.0f), base, absorption),
			  (1.0f - reflectance) * 0.78f);
	return glass;
}

static struct armada_vec3 rgb565_color(uint16_t pixel)
{
	return vec3((float)((pixel >> 11) & 0x1fU) / 31.0f,
		    (float)((pixel >> 5) & 0x3fU) / 63.0f,
		    (float)(pixel & 0x1fU) / 31.0f);
}

static struct armada_vec3 glass_pixel(const struct armada_scene *scene,
				      const struct armada_glass_surface *glass,
				      unsigned int x, unsigned int y)
{
	float sample_x = f_clamp((float)x + glass->offset_x, 0.0f,
				 (float)scene->width - 1.0f);
	float sample_y = f_clamp((float)y + glass->offset_y, 0.0f,
				 (float)scene->height - 1.0f);
	unsigned int left = (unsigned int)sample_x;
	unsigned int top = (unsigned int)sample_y;
	unsigned int right = left + (left + 1U < scene->width);
	unsigned int bottom = top + (top + 1U < scene->height);
	float fraction_x = sample_x - (float)left;
	float fraction_y = sample_y - (float)top;
	struct armada_vec3 upper = color_mix(
		rgb565_color(
			scene->background_pixels[(size_t)top * scene->width +
						 left]),
		rgb565_color(
			scene->background_pixels[(size_t)top * scene->width +
						 right]),
		fraction_x);
	struct armada_vec3 lower = color_mix(
		rgb565_color(
			scene->background_pixels[(size_t)bottom * scene->width +
						 left]),
		rgb565_color(
			scene->background_pixels[(size_t)bottom * scene->width +
						 right]),
		fraction_x);
	struct armada_vec3 transmitted = color_mix(upper, lower, fraction_y);

	return vec_add(glass->light,
		       vec3(transmitted.x * glass->transmission.x,
			    transmitted.y * glass->transmission.y,
			    transmitted.z * glass->transmission.z));
}

static float
letter_reflection_visibility(const struct armada_letter_state *letter)
{
	struct armada_vec3 axis_x = rotate_forward(
		letter, vec3(0.53f * letter->scale_x, 0.0f, 0.0f));
	struct armada_vec3 axis_y = rotate_forward(
		letter, vec3(0.0f, 0.77f * letter->scale_y, 0.0f));
	struct armada_vec3 axis_z =
		rotate_forward(letter, vec3(0.0f, 0.0f, 0.23f));
	float half_height = letter->scale * (f_abs(axis_x.y) + f_abs(axis_y.y) +
					     f_abs(axis_z.y));

	return smoothstep((letter->position.y - ARMADA_WATER_Y + half_height) /
			  (2.0f * half_height));
}

static void shadow_rasterize_triangle(const struct armada_scene *scene,
				      float *depth,
				      const struct triangle_vertex *first,
				      const struct triangle_vertex *second,
				      const struct triangle_vertex *third)
{
	float area = triangle_edge(first, second, third->x, third->y);
	int first_x =
		(int)f_max(f_min(first->x, f_min(second->x, third->x)), 0.0f);
	int last_x = (int)f_min(f_max(first->x, f_max(second->x, third->x)),
				(float)ARMADA_SHADOW_SIDE - 1.0f);
	int first_y =
		(int)f_max(f_min(first->y, f_min(second->y, third->y)), 0.0f);
	int last_y = (int)f_min(f_max(first->y, f_max(second->y, third->y)),
				(float)ARMADA_SHADOW_SIDE - 1.0f);
	float first_depth = vec_dot(first->world, scene->shadow_direction);
	float second_depth = vec_dot(second->world, scene->shadow_direction);
	float third_depth = vec_dot(third->world, scene->shadow_direction);
	int y;

	if (f_abs(area) < 0.001f)
		return;
	for (y = first_y; y <= last_y; ++y) {
		int x;

		for (x = first_x; x <= last_x; ++x) {
			float one = triangle_edge(second, third,
						  (float)x + 0.5f,
						  (float)y + 0.5f) /
				    area;
			float two = triangle_edge(third, first, (float)x + 0.5f,
						  (float)y + 0.5f) /
				    area;
			float three = 1.0f - one - two;
			float caster_depth;
			size_t index;

			if (one < -0.0001f || two < -0.0001f ||
			    three < -0.0001f)
				continue;
			caster_depth = first_depth * one + second_depth * two +
				       third_depth * three;
			index = (size_t)(unsigned int)y * ARMADA_SHADOW_SIDE +
				(unsigned int)x;
			depth[index] = f_max(depth[index], caster_depth);
		}
	}
}

static void prepare_shadow_light_samples(struct armada_scene *scene,
					 bool sunlight)
{
	/* Fixed symmetric disk quadrature: no temporal jitter in the penumbra. */
	static const float disk[ARMADA_LIGHT_SAMPLES][2] = {
		{ 0.28867513f, 0.00000000f },	{ -0.28867513f, 0.00000000f },
		{ -0.36868444f, 0.33774515f },	{ 0.36868444f, -0.33774515f },
		{ 0.05643306f, -0.64302564f },	{ -0.05643306f, 0.64302564f },
		{ 0.46470286f, 0.60612259f },	{ -0.46470286f, -0.60612259f },
		{ -0.85278689f, -0.15084599f }, { 0.85278689f, 0.15084599f },
		{ 0.80783419f, -0.51387799f },	{ -0.80783419f, 0.51387799f },
	};
	float radius = sunlight ? ARMADA_SUN_RADIUS : ARMADA_MOON_RADIUS;
	struct armada_vec3 center = vec_scale(scene->shadow_direction,
					      1.0f / scene->shadow_direction.z);
	unsigned int sample;

	scene->shadow_max_slope_u = 0.0f;
	scene->shadow_max_slope_v = 0.0f;
	for (sample = 0; sample < ARMADA_LIGHT_SAMPLES; ++sample) {
		/* Use the same projected disk as the sky, not a radius in guessed radians. */
		struct armada_vec3 direction =
			vec_add(center, vec3(disk[sample][0] * radius,
					     disk[sample][1] * radius, 0.0f));
		float forward = vec_dot(direction, scene->shadow_direction);
		float u = vec_dot(direction, scene->shadow_right) / forward;
		float v = vec_dot(direction, scene->shadow_up) / forward;

		scene->shadow_light_slopes[sample][0] = u;
		scene->shadow_light_slopes[sample][1] = v;
		scene->shadow_max_slope_u =
			f_max(scene->shadow_max_slope_u, f_abs(u));
		scene->shadow_max_slope_v =
			f_max(scene->shadow_max_slope_v, f_abs(v));
	}
}

static void render_water_shadows(struct armada_scene *scene)
{
	struct triangle_vertex vertices[ARMADA_STROKES][12];
	bool sunlight = scene->sun_visibility >= scene->moon_visibility;
	float minimum_u = FLT_MAX;
	float maximum_u = -FLT_MAX;
	float minimum_v = FLT_MAX;
	float maximum_v = -FLT_MAX;
	unsigned int letter;
	size_t index;

	scene->shadow_direction = sunlight ? scene->sun_direction :
					     scene->moon_direction;
	scene->shadow_strength =
		(sunlight ? scene->sun_visibility *
				    (0.48f + scene->daylight * 0.44f) :
			    scene->moon_visibility * scene->night * 0.28f) *
		smoothstep(scene->shadow_direction.y / 0.18f) *
		scene->cloud_transmittance;
	if (scene->shadow_strength <= 0.0f)
		return;
	/* Orthographic light coordinates: no camera position or point-light falloff. */
	scene->shadow_right = vec_normalize(vec3(
		scene->shadow_direction.z, 0.0f, -scene->shadow_direction.x));
	scene->shadow_up =
		triangle_cross(scene->shadow_direction, scene->shadow_right);
	prepare_shadow_light_samples(scene, sunlight);
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		struct armada_shadow_bounds *bounds =
			&scene->shadow_bounds[letter];
		unsigned int stroke;

		*bounds = (struct armada_shadow_bounds){
			.minimum_u = FLT_MAX,
			.maximum_u = -FLT_MAX,
			.minimum_v = FLT_MAX,
			.maximum_v = -FLT_MAX,
			.maximum_depth = -FLT_MAX,
		};
		scene->shadow_filter[letter] =
			vec_scale(color_mix(vec3(1.0f, 1.0f, 1.0f),
					    letter_base_color(letter),
					    ARMADA_GLASS_ABSORPTION),
				  0.92f);
		if (!scene->letters[letter].visible)
			continue;
		for (stroke = 0; stroke < glyph->count; ++stroke) {
			unsigned int stroke_index = glyph->first + stroke;
			const struct armada_prepared_stroke *prepared =
				&scene->prepared_strokes[stroke_index];
			unsigned int corner;

			for (corner = 0; corner < 12U; ++corner) {
				const struct armada_prism_profile *profile =
					&stroke_prism_profile[corner % 6U];
				struct triangle_vertex *vertex =
					&vertices[stroke_index][corner];
				float along =
					corner < 6U ?
						-prepared->half_length -
							0.095f :
						prepared->half_length + 0.095f;

				vertex->world = triangle_stroke_world(
					scene, letter, prepared, along,
					profile->across, profile->depth, false);
				vertex->x = vec_dot(vertex->world,
						    scene->shadow_right);
				vertex->y = vec_dot(vertex->world,
						    scene->shadow_up);
				minimum_u = f_min(minimum_u, vertex->x);
				maximum_u = f_max(maximum_u, vertex->x);
				minimum_v = f_min(minimum_v, vertex->y);
				maximum_v = f_max(maximum_v, vertex->y);
				bounds->minimum_u =
					f_min(bounds->minimum_u, vertex->x);
				bounds->maximum_u =
					f_max(bounds->maximum_u, vertex->x);
				bounds->minimum_v =
					f_min(bounds->minimum_v, vertex->y);
				bounds->maximum_v =
					f_max(bounds->maximum_v, vertex->y);
				bounds->maximum_depth =
					f_max(bounds->maximum_depth,
					      vec_dot(vertex->world,
						      scene->shadow_direction));
			}
		}
	}
	if (minimum_u == FLT_MAX) {
		scene->shadow_strength = 0.0f;
		return;
	}
	scene->shadow_min_u = minimum_u - 0.12f;
	scene->shadow_min_v = minimum_v - 0.12f;
	scene->shadow_scale_u = (float)(ARMADA_SHADOW_SIDE - 1U) /
				(maximum_u - minimum_u + 0.24f);
	scene->shadow_scale_v = (float)(ARMADA_SHADOW_SIDE - 1U) /
				(maximum_v - minimum_v + 0.24f);
	for (index = 0; index < (size_t)ARMADA_LETTERS * ARMADA_SHADOW_SIDE *
					ARMADA_SHADOW_SIDE;
	     ++index)
		scene->shadow_depth[index] = -FLT_MAX;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		float *depth = scene->shadow_depth +
			       (size_t)letter * ARMADA_SHADOW_SIDE *
				       ARMADA_SHADOW_SIDE;
		unsigned int stroke;

		if (!scene->letters[letter].visible)
			continue;
		{
			struct armada_shadow_bounds *bounds =
				&scene->shadow_bounds[letter];

			bounds->minimum_u =
				(bounds->minimum_u - scene->shadow_min_u) *
					scene->shadow_scale_u -
				0.5f;
			bounds->maximum_u =
				(bounds->maximum_u - scene->shadow_min_u) *
					scene->shadow_scale_u -
				0.5f;
			bounds->minimum_v =
				(bounds->minimum_v - scene->shadow_min_v) *
					scene->shadow_scale_v -
				0.5f;
			bounds->maximum_v =
				(bounds->maximum_v - scene->shadow_min_v) *
					scene->shadow_scale_v -
				0.5f;
		}
		for (stroke = 0; stroke < glyph->count; ++stroke) {
			struct triangle_vertex *projected =
				vertices[glyph->first + stroke];
			unsigned int corner;
			unsigned int face;

			for (corner = 0; corner < 12U; ++corner) {
				projected[corner].x = (projected[corner].x -
						       scene->shadow_min_u) *
						      scene->shadow_scale_u;
				projected[corner].y = (projected[corner].y -
						       scene->shadow_min_v) *
						      scene->shadow_scale_v;
			}
			for (face = 0; face < ARRAY_SIZE(stroke_prism_faces);
			     ++face)
				shadow_rasterize_triangle(
					scene, depth,
					&projected[stroke_prism_faces[face][0]],
					&projected[stroke_prism_faces[face][1]],
					&projected[stroke_prism_faces[face][2]]);
		}
	}
}

struct armada_shadow_sample {
	float coverage;
	float separation;
};

static struct armada_shadow_sample shadow_sample(const float *depth, float u,
						 float v, float receiver_depth)
{
	struct armada_shadow_sample result = { 0.0f, 0.0f };
	int left;
	int top;
	float fraction_u;
	float fraction_v;
	unsigned int row;

	/* Out-of-atlas samples remain lit; do not clamp or renormalize their weight. */
	if (u < -1.0f || v < -1.0f || u >= (float)ARMADA_SHADOW_SIDE ||
	    v >= (float)ARMADA_SHADOW_SIDE)
		return result;
	left = floor_to_int(u);
	top = floor_to_int(v);
	fraction_u = u - (float)left;
	fraction_v = v - (float)top;
	for (row = 0; row < 2U; ++row) {
		int y = top + (int)row;
		float vertical = row ? fraction_v : 1.0f - fraction_v;
		unsigned int column;

		if (y < 0 || y >= (int)ARMADA_SHADOW_SIDE)
			continue;
		for (column = 0; column < 2U; ++column) {
			int x = left + (int)column;
			float horizontal = column ? fraction_u :
						    1.0f - fraction_u;
			float caster_depth;
			float weight;

			if (x < 0 || x >= (int)ARMADA_SHADOW_SIDE)
				continue;
			caster_depth = depth[(size_t)y * ARMADA_SHADOW_SIDE +
					     (unsigned int)x];
			if (caster_depth <= receiver_depth)
				continue;
			weight = horizontal * vertical;
			result.coverage += weight;
			result.separation +=
				weight * (caster_depth - receiver_depth);
		}
	}
	return result;
}

static float shadow_letter_coverage(const struct armada_scene *scene,
				    unsigned int letter, float u, float v,
				    float receiver_depth)
{
	const float *depth = scene->shadow_depth + (size_t)letter *
							   ARMADA_SHADOW_SIDE *
							   ARMADA_SHADOW_SIDE;
	const struct armada_shadow_bounds *bounds =
		&scene->shadow_bounds[letter];
	float maximum_separation = bounds->maximum_depth - receiver_depth;
	float radius_u;
	float radius_v;
	float blocker_weight = 0.0f;
	float blocker_separation = 0.0f;
	float separation;
	float coverage = 0.0f;
	unsigned int sample;

	if (maximum_separation <= 0.0f)
		return 0.0f;
	radius_u = maximum_separation * scene->shadow_max_slope_u *
		   scene->shadow_scale_u;
	radius_v = maximum_separation * scene->shadow_max_slope_v *
		   scene->shadow_scale_v;
	if (u + radius_u < bounds->minimum_u - 1.0f ||
	    u - radius_u > bounds->maximum_u + 1.0f ||
	    v + radius_v < bounds->minimum_v - 1.0f ||
	    v - radius_v > bounds->maximum_v + 1.0f)
		return 0.0f;
	if (radius_u < 0.5f && radius_v < 0.5f)
		return shadow_sample(depth, u, v, receiver_depth).coverage;
	/* Search outside the hard silhouette too, otherwise its outer penumbra is lost. */
	for (sample = 0; sample < ARMADA_LIGHT_SAMPLES; ++sample) {
		struct armada_shadow_sample blocker = shadow_sample(
			depth,
			u + maximum_separation *
					scene->shadow_light_slopes[sample][0] *
					scene->shadow_scale_u,
			v + maximum_separation *
					scene->shadow_light_slopes[sample][1] *
					scene->shadow_scale_v,
			receiver_depth);

		blocker_weight += blocker.coverage;
		blocker_separation += blocker.separation;
	}
	if (blocker_weight <= 0.0f)
		return 0.0f;
	separation = blocker_separation / blocker_weight;
	for (sample = 0; sample < ARMADA_LIGHT_SAMPLES; ++sample)
		coverage +=
			shadow_sample(depth,
				      u + separation *
						      scene->shadow_light_slopes
							      [sample][0] *
						      scene->shadow_scale_u,
				      v + separation *
						      scene->shadow_light_slopes
							      [sample][1] *
						      scene->shadow_scale_v,
				      receiver_depth)
				.coverage;
	return coverage / (float)ARMADA_LIGHT_SAMPLES;
}

static struct armada_vec3
water_light_transmission(const struct armada_scene *scene,
			 struct armada_vec3 point)
{
	struct armada_vec3 transmission = vec3(1.0f, 1.0f, 1.0f);
	float u;
	float v;
	float receiver_depth;
	unsigned int letter;

	if (scene->shadow_strength <= 0.0f)
		return transmission;
	u = (vec_dot(point, scene->shadow_right) - scene->shadow_min_u) *
		    scene->shadow_scale_u -
	    0.5f;
	v = (vec_dot(point, scene->shadow_up) - scene->shadow_min_v) *
		    scene->shadow_scale_v -
	    0.5f;
	/* Compare at the actual wavy-water hit, so submerged casters cannot shade it. */
	receiver_depth = vec_dot(point, scene->shadow_direction) + 0.005f;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		float coverage = shadow_letter_coverage(scene, letter, u, v,
							receiver_depth);

		/* One filter per letter, not per overlapping triangle or stroke. */
		transmission = color_filter(
			transmission,
			color_mix(vec3(1.0f, 1.0f, 1.0f),
				  scene->shadow_filter[letter],
				  coverage * scene->shadow_strength));
	}
	return transmission;
}

static bool triangle_water_at_pixel(const struct armada_scene *scene,
				    unsigned int x, unsigned int y,
				    float *surface_z)
{
	size_t index = (size_t)y * scene->width + x;
	float cached = scene->water_surface_z[index];
	struct armada_vec3 direction;
	float distance;

	if (cached != 0.0f) {
		if (cached < 0.0f)
			return false;
		*surface_z = cached;
		return true;
	}
	direction = scene->full_rays[index];
	if (!water_intersection(scene, camera_position(), direction,
				&distance)) {
		scene->water_surface_z[index] = -1.0f;
		return false;
	}
	*surface_z = distance * direction.z;
	scene->water_surface_z[index] = *surface_z;
	return true;
}

static inline bool triangle_block_empty(const struct triangle_vertex *first,
					const struct triangle_vertex *second,
					const struct triangle_vertex *third,
					float area, int first_x, int first_y,
					int last_x, int last_y)
{
	const float corner_x[2] = { (float)first_x + 0.5f,
				    (float)last_x + 0.5f };
	const float corner_y[2] = { (float)first_y + 0.5f,
				    (float)last_y + 0.5f };
	float minimum_one = 0.0f;
	float maximum_one = 0.0f;
	float minimum_two = 0.0f;
	float maximum_two = 0.0f;
	unsigned int corner;

	for (corner = 0; corner < 4U; ++corner) {
		float one = triangle_edge(second, third, corner_x[corner & 1U],
					  corner_y[corner >> 1U]) /
			    area;
		float two = triangle_edge(third, first, corner_x[corner & 1U],
					  corner_y[corner >> 1U]) /
			    area;

		if (corner == 0U) {
			minimum_one = maximum_one = one;
			minimum_two = maximum_two = two;
		} else {
			minimum_one = f_min(minimum_one, one);
			maximum_one = f_max(maximum_one, one);
			minimum_two = f_min(minimum_two, two);
			maximum_two = f_max(maximum_two, two);
		}
	}
	/* Independent minima bound 1 - one - two despite cancellation. */
	return maximum_one < -0.0001f || maximum_two < -0.0001f ||
	       ((1.0f - minimum_one) - minimum_two) < -0.0001f;
}

static void triangle_rasterize(struct armada_scene *scene,
			       const struct triangle_vertex *first,
			       const struct triangle_vertex *second,
			       const struct triangle_vertex *third,
			       unsigned int letter, bool reflection)
{
	float area = triangle_edge(first, second, third->x, third->y);
	float minimum_x;
	float maximum_x;
	float minimum_y;
	float maximum_y;
	struct armada_vec3 normal;
	struct armada_vec3 midpoint;
	struct armada_glass_surface glass;
	float reflection_visibility = 1.0f;
	int first_x;
	int last_x;
	int first_y;
	int last_y;
	int tile_y;

	if (f_abs(area) < 0.001f)
		return;
	if (reflection) {
		reflection_visibility =
			letter_reflection_visibility(&scene->letters[letter]);
		if (reflection_visibility <= 0.0f)
			return;
	}
	minimum_x = f_min(first->x, f_min(second->x, third->x));
	maximum_x = f_max(first->x, f_max(second->x, third->x));
	minimum_y = f_min(first->y, f_min(second->y, third->y));
	maximum_y = f_max(first->y, f_max(second->y, third->y));
	if (maximum_x < 0.0f || maximum_y < 0.0f ||
	    minimum_x >= (float)scene->width ||
	    minimum_y >= (float)scene->height)
		return;
	normal = vec_normalize(
		triangle_cross(vec_sub(second->world, first->world),
			       vec_sub(third->world, first->world)));
	if (reflection)
		normal = vec_scale(normal, -1.0f);
	midpoint = vec_scale(vec_add(vec_add(first->world, second->world),
				     third->world),
			     1.0f / 3.0f);
	if (vec_dot(normal, vec_sub(camera_position(), midpoint)) <= 0.0f)
		return;
	glass = prepare_glass_surface(scene, letter, normal, midpoint,
				      reflection);
	first_x = (int)f_max(minimum_x, 0.0f);
	last_x = (int)f_min(maximum_x, (float)scene->width - 1.0f);
	first_y = (int)f_max(minimum_y, 0.0f);
	last_y = (int)f_min(maximum_y, (float)scene->height - 1.0f);
	for (tile_y = first_y; tile_y <= last_y;
	     tile_y += ARMADA_TRIANGLE_TILE_SIDE) {
		int end_y = tile_y + (ARMADA_TRIANGLE_TILE_SIDE - 1) < last_y ?
				    tile_y + (ARMADA_TRIANGLE_TILE_SIDE - 1) :
				    last_y;
		int tile_x;

		for (tile_x = first_x; tile_x <= last_x;
		     tile_x += ARMADA_TRIANGLE_TILE_SIDE) {
			int end_x =
				tile_x + (ARMADA_TRIANGLE_TILE_SIDE - 1) <
						last_x ?
					tile_x + (ARMADA_TRIANGLE_TILE_SIDE -
						  1) :
					last_x;
			int y;

			if ((end_x - tile_x + 1) * (end_y - tile_y + 1) > 4 &&
			    triangle_block_empty(first, second, third, area,
						 tile_x, tile_y, end_x, end_y))
				continue;
			for (y = tile_y; y <= end_y; ++y) {
				int x;

				for (x = tile_x; x <= end_x; ++x) {
					float one =
						triangle_edge(second, third,
							      (float)x + 0.5f,
							      (float)y + 0.5f) /
						area;
					float two =
						triangle_edge(third, first,
							      (float)x + 0.5f,
							      (float)y + 0.5f) /
						area;
					float three = 1.0f - one - two;
					float inverse_z;
					size_t index;

					if (one < -0.0001f || two < -0.0001f ||
					    three < -0.0001f)
						continue;
					inverse_z = first->inverse_z * one +
						    second->inverse_z * two +
						    third->inverse_z * three;
					if (inverse_z <= 0.0f)
						continue;
					index = (size_t)(unsigned int)y *
							scene->width +
						(unsigned int)x;
					if (reflection) {
						float unused_surface_z;

						if (inverse_z <=
							    scene->reflection_depth
								    [index] ||
						    !triangle_water_at_pixel(
							    scene,
							    (unsigned int)x,
							    (unsigned int)y,
							    &unused_surface_z))
							continue;
						scene->reflection_depth[index] =
							inverse_z;
						scene->pixels
							[index] = triangle_mix_rgb565(
							scene->pixels[index],
							vec_scale(
								glass_pixel(
									scene,
									&glass,
									(unsigned int)
										x,
									(unsigned int)
										y),
								0.58f),
							(0.10f +
							 0.24f * scene->daylight) *
								reflection_visibility);
					} else {
						float surface_z;

						if (inverse_z <=
						    scene->triangle_depth[index])
							continue;
						if (triangle_water_at_pixel(
							    scene,
							    (unsigned int)x,
							    (unsigned int)y,
							    &surface_z) &&
						    1.0f / inverse_z >
							    surface_z)
							continue;
						scene->triangle_depth[index] =
							inverse_z;
						scene->pixels[index] =
							color_to_rgb565(glass_pixel(
								scene, &glass,
								(unsigned int)x,
								(unsigned int)
									y));
					}
				}
			}
		}
	}
}

static void
triangle_rasterize_stroke(struct armada_scene *scene, unsigned int letter,
			  const struct armada_prepared_stroke *stroke,
			  bool reflection)
{
	/* A flat front and two chamfer faces catch the travelling sky light. */
	struct triangle_vertex vertices[12];
	float along[2] = { -stroke->half_length - 0.095f,
			   stroke->half_length + 0.095f };
	unsigned int vertex;
	unsigned int face;

	for (vertex = 0; vertex < ARRAY_SIZE(vertices); ++vertex) {
		const struct armada_prism_profile *profile =
			&stroke_prism_profile[vertex %
					      ARRAY_SIZE(stroke_prism_profile)];

		vertices[vertex].world = triangle_stroke_world(
			scene, letter, stroke,
			along[vertex / ARRAY_SIZE(stroke_prism_profile)],
			profile->across, profile->depth, reflection);
		if (!triangle_project(scene, &vertices[vertex], reflection))
			return;
	}
	for (face = 0; face < ARRAY_SIZE(stroke_prism_faces); ++face)
		triangle_rasterize(scene,
				   &vertices[stroke_prism_faces[face][0]],
				   &vertices[stroke_prism_faces[face][1]],
				   &vertices[stroke_prism_faces[face][2]],
				   letter, reflection);
}

static void triangle_rasterize_letters(struct armada_scene *scene,
				       bool reflection)
{
	unsigned int letter;

	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		unsigned int index;

		if (!scene->letters[letter].visible)
			continue;
		if (reflection && letter_reflection_visibility(
					  &scene->letters[letter]) <= 0.0f)
			continue;
		for (index = 0; index < glyph->count; ++index)
			triangle_rasterize_stroke(
				scene, letter,
				&scene->prepared_strokes[glyph->first + index],
				reflection);
	}
}

static void render_scene(struct armada_scene *scene)
{
	unsigned int y;

	memset(scene->triangle_depth, 0,
	       (size_t)scene->width * scene->height *
		       sizeof(*scene->triangle_depth));
	memset(scene->reflection_depth, 0,
	       (size_t)scene->width * scene->height *
		       sizeof(*scene->reflection_depth));
	memset(scene->water_surface_z, 0,
	       (size_t)scene->width * scene->height *
		       sizeof(*scene->water_surface_z));
	render_water_shadows(scene);
	for (y = 0; y < scene->low_height; ++y) {
		unsigned int x;

		for (x = 0; x < scene->low_width; ++x) {
			struct armada_vec3 color = triangle_background_sample(
				scene,
				scene->rays[(size_t)y * scene->low_width + x]);

			scene->low_pixels[(size_t)y * scene->low_width + x] =
				color_to_rgb565(color);
		}
	}
	upscale_background(scene);
	refine_celestial_bodies(scene);
	/* Each glass pass reads a complete current-frame background, never itself. */
	memcpy(scene->background_pixels, scene->pixels,
	       (size_t)scene->width * scene->height * sizeof(*scene->pixels));
	triangle_rasterize_letters(scene, true);
	memcpy(scene->background_pixels, scene->pixels,
	       (size_t)scene->width * scene->height * sizeof(*scene->pixels));
	triangle_rasterize_letters(scene, false);
}

static void put_pixel(struct armada_scene *scene, int x, int y, uint16_t color)
{
	if (x >= 0 && y >= 0 && (unsigned int)x < scene->width &&
	    (unsigned int)y < scene->height)
		scene->pixels[(size_t)y * scene->width + (unsigned int)x] =
			color;
}

static const uint8_t *font_rows(char character)
{
	static const uint8_t alphabet[36][7] = {
		{ 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 30, 17, 17, 17, 30 },
		{ 15, 16, 16, 16, 16, 16, 15 }, { 30, 17, 17, 17, 17, 17, 30 },
		{ 31, 16, 30, 16, 16, 16, 31 }, { 31, 16, 30, 16, 16, 16, 16 },
		{ 15, 16, 16, 19, 17, 17, 15 }, { 17, 17, 31, 17, 17, 17, 17 },
		{ 31, 4, 4, 4, 4, 4, 31 },	{ 1, 1, 1, 1, 17, 17, 14 },
		{ 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
		{ 17, 27, 21, 21, 17, 17, 17 }, { 17, 25, 25, 21, 19, 19, 17 },
		{ 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
		{ 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 },
		{ 15, 16, 16, 14, 1, 1, 30 },	{ 31, 4, 4, 4, 4, 4, 4 },
		{ 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },
		{ 17, 17, 17, 21, 21, 27, 17 }, { 17, 17, 10, 4, 10, 17, 17 },
		{ 17, 17, 10, 4, 4, 4, 4 },	{ 31, 1, 2, 4, 8, 16, 31 },
		{ 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
		{ 14, 17, 1, 2, 4, 8, 31 },	{ 30, 1, 1, 14, 1, 1, 30 },
		{ 2, 6, 10, 18, 31, 2, 2 },	{ 31, 16, 30, 1, 1, 17, 14 },
		{ 6, 8, 16, 30, 17, 17, 14 },	{ 31, 1, 2, 4, 8, 8, 8 },
		{ 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 },
	};
	static const uint8_t space[7];

	if (character >= 'a' && character <= 'z')
		character = (char)(character - 'a' + 'A');
	if (character >= 'A' && character <= 'Z')
		return alphabet[(unsigned int)(character - 'A')];
	if (character >= '0' && character <= '9')
		return alphabet[26U + (unsigned int)(character - '0')];
	return space;
}

static void draw_character(struct armada_scene *scene, int x, int y,
			   char character, unsigned int scale, uint16_t color)
{
	const uint8_t *rows = font_rows(character);
	unsigned int row;
	unsigned int column;
	unsigned int dx;
	unsigned int dy;

	if (character == ':' || character == '.') {
		unsigned int first = character == ':' ? 2U : 5U;

		put_pixel(scene, x + (int)(2U * scale),
			  y + (int)(first * scale), color);
		if (character == ':')
			put_pixel(scene, x + (int)(2U * scale),
				  y + (int)(5U * scale), color);
		return;
	}
	if (character == '-') {
		for (column = 0; column < 5U * scale; ++column)
			put_pixel(scene, x + (int)column, y + (int)(3U * scale),
				  color);
		return;
	}
	for (row = 0; row < 7U; ++row)
		for (column = 0; column < 5U; ++column)
			if (rows[row] & (1U << (4U - column)))
				for (dy = 0; dy < scale; ++dy)
					for (dx = 0; dx < scale; ++dx)
						put_pixel(
							scene,
							x + (int)(column *
									  scale +
								  dx),
							y + (int)(row * scale +
								  dy),
							color);
}

static int text_width(const char *text, unsigned int scale)
{
	return text && *text ? (int)((strlen(text) * 6U - 1U) * scale) : 0;
}

static void draw_text(struct armada_scene *scene, int x, int y,
		      const char *text, unsigned int scale, uint16_t color)
{
	while (*text) {
		draw_character(scene, x, y, *text, scale, color);
		x += (int)(6U * scale);
		++text;
	}
}

static void draw_centered(struct armada_scene *scene, int y, const char *text,
			  unsigned int scale, uint16_t color)
{
	draw_text(scene, ((int)scene->width - text_width(text, scale)) / 2, y,
		  text, scale, color);
}

static void compute_outputs(uint32_t frame, struct armada_outputs *outputs)
{
	struct armada_morse_symbol symbol;

	outputs->keypad = false;
	outputs->rumble = false;
	outputs->cue_id = 0;
	outputs->lcd_level = cycle_lcd_level(frame);
	if (!morse_symbol_for_frame(frame, &symbol))
		return;
	outputs->keypad = true;
	outputs->rumble = true;
	outputs->cue_id = symbol.cue_id;
}

static void draw_overlay(struct armada_scene *scene,
			 const struct armada_metrics *metrics)
{
	unsigned int scale = scene->width <= 128U ? 1U : 2U;
	uint16_t cyan = color_to_rgb565(vec3(0.25f, 0.88f, 1.0f));
	uint16_t white = color_to_rgb565(vec3(0.90f, 0.96f, 1.0f));
	char line[48];

	draw_centered(scene, (int)scene->height / 12, "FPLINUX ARMADA", 1,
		      white);
	if (scene->frame >= ARMADA_FINAL_METRICS && metrics && metrics->valid) {
		snprintf(line, sizeof(line), "AVG %u.%u FPS",
			 metrics->average_fps_tenths / 10U,
			 metrics->average_fps_tenths % 10U);
		draw_centered(scene, (int)scene->height * 3 / 4, line, 1, cyan);
		snprintf(line, sizeof(line), "MIN %u.%u  MAX %u MS",
			 metrics->minimum_fps_tenths / 10U,
			 metrics->minimum_fps_tenths % 10U,
			 metrics->maximum_frame_us / 1000U);
		draw_centered(scene, (int)scene->height * 3 / 4 + 10, line, 1,
			      white);
		draw_centered(scene, (int)scene->height - 12, "BACK EXITS", 1,
			      white);
	} else {
		draw_centered(scene, (int)scene->height - (int)(12U * scale),
			      "DISPLAY KEYS HAPTICS", 1, cyan);
	}
}

struct armada_scene *armada_scene_create(unsigned int width,
					 unsigned int height, uint16_t *pixels)
{
	struct armada_scene *scene;
	unsigned int y;

	if (!pixels || width == 0U || height == 0U || width > 240U ||
	    height > 320U) {
		errno = EINVAL;
		return NULL;
	}
	scene = calloc(1, sizeof(*scene));
	if (!scene)
		return NULL;
	scene->width = width;
	scene->height = height;
	scene->low_width = (width + ARMADA_BACKGROUND_SCALE - 1U) /
			   ARMADA_BACKGROUND_SCALE;
	scene->low_height = (height + ARMADA_BACKGROUND_SCALE - 1U) /
			    ARMADA_BACKGROUND_SCALE;
	scene->pixels = pixels;
	scene->background_pixels = calloc((size_t)width * height,
					  sizeof(*scene->background_pixels));
	scene->low_pixels = calloc((size_t)scene->low_width * scene->low_height,
				   sizeof(*scene->low_pixels));
	scene->shadow_depth =
		calloc((size_t)ARMADA_LETTERS * ARMADA_SHADOW_SIDE *
			       ARMADA_SHADOW_SIDE,
		       sizeof(*scene->shadow_depth));
	scene->rays = calloc((size_t)scene->low_width * scene->low_height,
			     sizeof(*scene->rays));
	scene->full_rays =
		calloc((size_t)width * height, sizeof(*scene->full_rays));
	scene->triangle_depth =
		calloc((size_t)width * height, sizeof(*scene->triangle_depth));
	scene->reflection_depth = calloc((size_t)width * height,
					 sizeof(*scene->reflection_depth));
	scene->water_surface_z =
		calloc((size_t)width * height, sizeof(*scene->water_surface_z));
	if (!scene->background_pixels || !scene->low_pixels ||
	    !scene->shadow_depth || !scene->rays || !scene->full_rays ||
	    !scene->triangle_depth || !scene->reflection_depth ||
	    !scene->water_surface_z) {
		free(scene->water_surface_z);
		free(scene->reflection_depth);
		free(scene->triangle_depth);
		free(scene->full_rays);
		free(scene->rays);
		free(scene->shadow_depth);
		free(scene->low_pixels);
		free(scene->background_pixels);
		free(scene);
		return NULL;
	}
	prepare_stroke_geometry(scene);
	for (y = 0; y < scene->height; ++y) {
		unsigned int x;

		for (x = 0; x < scene->width; ++x)
			scene->full_rays[(size_t)y * scene->width + x] =
				output_ray(scene, x, y);
	}
	for (y = 0; y < scene->low_height; ++y) {
		unsigned int x;

		for (x = 0; x < scene->low_width; ++x) {
			float denominator = (float)scene->low_height * 0.95f;
			struct armada_vec3 direction =
				vec3(((float)(2U * x + 1U) -
				      (float)scene->low_width) /
					     denominator,
				     ((float)scene->low_height * 1.125f -
				      (float)(2U * y + 1U)) /
					     denominator,
				     1.0f);

			scene->rays[(size_t)y * scene->low_width + x] =
				vec_normalize(direction);
		}
	}
	return scene;
}

void armada_scene_destroy(struct armada_scene *scene)
{
	if (!scene)
		return;
	free(scene->water_surface_z);
	free(scene->reflection_depth);
	free(scene->triangle_depth);
	free(scene->full_rays);
	free(scene->rays);
	free(scene->shadow_depth);
	free(scene->low_pixels);
	free(scene->background_pixels);
	free(scene);
}

void armada_scene_render(struct armada_scene *scene, uint32_t frame,
			 const struct armada_metrics *metrics,
			 struct armada_outputs *outputs)
{
	if (!scene || !outputs)
		return;
	scene->frame = frame % ARMADA_DURATION_FRAMES;
	compute_outputs(scene->frame, outputs);
	update_storyboard(scene);
	render_scene(scene);
	draw_overlay(scene, metrics);
}
