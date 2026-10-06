// SPDX-License-Identifier: GPL-2.0-only
#include "armada-renderer.h"
#include "armada-math.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARMADA_STROKES 20U
#define ARMADA_NEAR_WATER_DISTANCE 13.0f
#define ARMADA_WATER_FAR_DISTANCE 80.0f
#define ARMADA_CLOUD_HEIGHT 7.2f
#define ARMADA_SUN_RADIUS 0.046f
#define ARMADA_MOON_RADIUS 0.040f
#define ARMADA_FINAL_METRICS (32U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_BACKGROUND_SCALE 4U
#define ARMADA_TRIANGLE_TILE_SIDE 8
#define ARMADA_GLASS_IOR 1.50f
#define ARMADA_GLASS_ABSORPTION 0.72f
#define ARMADA_SHADOW_SIDE 128U
#define ARMADA_LIGHT_SAMPLES 12U

struct armada_prepared_stroke {
	float midpoint_x;
	float midpoint_y;
	float unit_x;
	float unit_y;
	float half_length;
};

struct armada_shadow_bounds {
	float minimum_u;
	float maximum_u;
	float minimum_v;
	float maximum_v;
	float maximum_depth;
};

struct armada_renderer {
	const struct fplinux_font *font;
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
	const struct armada_frame *frame;
	float cloud_transmittance;
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

static float vec_length(struct armada_vec3 v)
{
	float squared = vec_dot(v, v);
	return squared > 0.0000001f ? squared * fast_inverse_sqrt(squared) :
				      0.0f;
}
static struct armada_vec3 vec_reflect(struct armada_vec3 direction,
				      struct armada_vec3 normal)
{
	return vec_sub(direction,
		       vec_scale(normal, 2.0f * vec_dot(direction, normal)));
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

static float cloud_field(const struct armada_renderer *renderer, float x,
			 float y)
{
	/* Both octaves are one wind field: toward -x and -z in world space. */
	float drift = (float)renderer->frame->number * 0.0019f;
	float wind_x = x + drift;
	float wind_z = y + drift * 0.48f;
	float broad = value_noise(wind_x, wind_z);
	float detail =
		value_noise(wind_x * 2.03f + 7.3f, wind_z * 2.07f - 3.1f);

	return broad * 0.72f + detail * 0.28f;
}

static float cloud_plane_coverage(const struct armada_renderer *renderer,
				  float x, float z)
{
	float density = smoothstep(
		(cloud_field(renderer, x * 0.22f, z * 0.16f) - 0.54f) / 0.20f);

	return f_clamp(density * 0.68f, 0.0f, 0.68f);
}

static float cloud_coverage_ray(const struct armada_renderer *renderer,
				struct armada_vec3 origin,
				struct armada_vec3 direction)
{
	float travel =
		(ARMADA_CLOUD_HEIGHT - origin.y) / f_max(direction.y, 0.12f);
	struct armada_vec3 projected =
		vec_add(origin, vec_scale(direction, travel));
	float altitude_envelope =
		smoothstep(f_clamp((direction.y - 0.015f) / 0.17f, 0.0f, 1.0f));

	return cloud_plane_coverage(renderer, projected.x, projected.z) *
	       altitude_envelope;
}

static float surface_cloud_coverage(const struct armada_renderer *renderer,
				    struct armada_vec3 point)
{
	float light_height = renderer->frame->environment.celestial_direction.y;
	float projection_weight =
		smoothstep(f_clamp((light_height - 0.04f) / 0.20f, 0.0f, 1.0f));
	float travel =
		(ARMADA_CLOUD_HEIGHT - point.y) / f_max(light_height, 0.12f);
	struct armada_vec3 projected = vec_add(
		point,
		vec_scale(renderer->frame->environment.celestial_direction,
			  travel));
	float projected_coverage =
		cloud_plane_coverage(renderer, projected.x, projected.z);
	float global_coverage = f_clamp(
		(1.0f - renderer->cloud_transmittance) / 0.20f, 0.0f, 0.68f);

	return global_coverage * (1.0f - projection_weight) +
	       projected_coverage * projection_weight;
}
static void prepare_stroke_geometry(struct armada_renderer *renderer)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(strokes); ++index) {
		const struct armada_stroke *stroke = &strokes[index];
		struct armada_prepared_stroke *prepared =
			&renderer->prepared_strokes[index];
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

static struct armada_vec3 star_color(const struct armada_renderer *renderer,
				     struct armada_vec3 direction)
{
	float projection = f_max(direction.z, 0.05f);
	int cell_x;
	int cell_y;
	uint32_t hash;
	unsigned int twinkle;
	float visibility = 0.06f + renderer->frame->environment.night * 0.94f;

	if (direction.y <= 0.0f)
		return vec3(0.0f, 0.0f, 0.0f);
	/* Sky and rough-water reflection address this same projected star field. */
	cell_x = floor_to_int((direction.x / projection + 1.8f) * 96.0f);
	cell_y = floor_to_int((direction.y / projection + 0.2f) * 96.0f);
	hash = hash32((uint32_t)cell_x * UINT32_C(73856093) ^
		      (uint32_t)cell_y * UINT32_C(19349663));
	twinkle =
		(unsigned int)((renderer->frame->number / 18U + (hash >> 11)) &
			       3U);
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

static float celestial_body_edge(const struct armada_renderer *renderer,
				 struct armada_vec3 direction,
				 struct armada_vec3 body, float radius)
{
	float pixel = 2.0f / ((float)renderer->height * 0.95f);
	float inner = f_max(radius - pixel, radius * 0.55f);
	float radius_squared = radius * radius;
	float inner_squared = inner * inner;
	float distance_squared =
		celestial_plane_distance_squared(direction, body);

	return smoothstep((radius_squared - distance_squared) /
			  (radius_squared - inner_squared));
}

static struct armada_vec3 sun_body_color(const struct armada_renderer *renderer)
{
	float high_sun = smoothstep(
		f_clamp(renderer->frame->environment.sun_direction.y / 0.42f,
			0.0f, 1.0f));

	return color_mix(vec3(1.0f, 0.34f, 0.08f), vec3(1.0f, 0.97f, 0.76f),
			 high_sun);
}

static struct armada_vec3 sky_color(const struct armada_renderer *renderer,
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
	struct armada_vec3 color = color_mix(
		night, day,
		f_clamp(renderer->frame->environment.daylight, 0.0f, 1.0f));
	float twilight =
		f_clamp(1.0f - renderer->frame->environment.daylight -
				renderer->frame->environment.night * 0.35f,
			0.0f, 1.0f);
	float sun_body =
		vec_dot(direction, renderer->frame->environment.sun_direction);
	float sun_edge = celestial_body_edge(
		renderer, direction, renderer->frame->environment.sun_direction,
		ARMADA_SUN_RADIUS);
	float moon_edge =
		celestial_body_edge(renderer, direction,
				    renderer->frame->environment.moon_direction,
				    ARMADA_MOON_RADIUS);
	float coverage = cloud_coverage_ray(renderer, origin, direction);
	float cloud_opacity = coverage * 0.68f;

	if (twilight > 0.0f)
		color = color_mix(color, vec3(0.44f, 0.105f, 0.065f),
				  twilight * (1.0f - height) * 0.72f);
	if (sun_edge > 0.0f) {
		color = vec_add(
			color,
			color_filter(
				vec_scale(vec_sub(sun_body_color(renderer),
						  color),
					  sun_edge * renderer->frame->environment
							     .sun_visibility),
				light_transmission));
	}
	if (moon_edge > 0.0f)
		color = vec_add(
			color,
			color_filter(
				vec_scale(vec_sub(vec3(0.68f, 0.80f, 1.0f),
						  color),
					  moon_edge *
						  renderer->frame->environment
							  .moon_visibility),
				light_transmission));
	color = vec_add(color, star_color(renderer, direction));
	{
		float light_facing = smoothstep(f_clamp(
			(vec_dot(direction, renderer->frame->environment
						    .celestial_direction) -
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
			color_mix(night_cloud, day_cloud,
				  renderer->frame->environment.daylight);

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

static void add_letter_ripples(const struct armada_renderer *renderer, float x,
			       float z, struct armada_water_sample *sample)
{
	unsigned int index;

	for (index = 0; index < renderer->frame->ripple_count; ++index) {
		const struct armada_ripple *ripple =
			&renderer->frame->ripples[index];

		add_ripple_ring(sample, x - ripple->origin.x,
				z - ripple->origin.z, ripple->radius,
				ripple->amplitude);
	}
}

static struct armada_water_sample
water_sample(const struct armada_renderer *renderer, float x, float z)
{
	const float radians_per_phase = 0.09817477f;
	float phase_one =
		x * 6.0f + z * 3.0f + (float)renderer->frame->number * 0.36f;
	float phase_two =
		x * -4.0f + z * 8.0f - (float)renderer->frame->number * 0.29f;
	float phase_three =
		x * 10.0f - z * 5.0f + (float)renderer->frame->number * 0.48f;
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

	add_letter_ripples(renderer, x, z, &sample);
	return sample;
}

static bool water_intersection(const struct armada_renderer *renderer,
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
			water_sample(renderer, current.x, current.z);
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

static struct armada_vec3 water_normal(const struct armada_renderer *renderer,
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
	float ripple_one =
		wave_sample_f(point.x * 18.0f + point.z * 11.0f +
			      (float)renderer->frame->number * 0.76f + 16.0f);
	float ripple_two =
		wave_sample_f(point.x * -15.0f + point.z * 21.0f -
			      (float)renderer->frame->number * 0.63f + 16.0f);
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
triangle_background_water(const struct armada_renderer *renderer,
			  struct armada_vec3 point,
			  struct armada_vec3 direction, float distance,
			  struct armada_water_sample sample,
			  struct armada_vec3 light_transmission)
{
	struct armada_vec3 normal =
		water_normal(renderer, point, sample, distance);
	struct armada_vec3 reflected = vec_reflect(direction, normal);
	struct armada_vec3 view = vec_scale(direction, -1.0f);
	float facing = f_clamp(vec_dot(normal, view), 0.0f, 1.0f);
	float grazing = 1.0f - facing;
	float fresnel = 0.040f + 0.960f * grazing * grazing * grazing *
					 grazing * grazing;
	float absorption =
		f_clamp(distance / ARMADA_WATER_FAR_DISTANCE, 0.0f, 1.0f);
	struct armada_vec3 near_water = color_mix(
		vec3(0.012f, 0.125f, 0.16f), vec3(0.025f, 0.30f, 0.36f),
		renderer->frame->environment.daylight);
	struct armada_vec3 deep_water = color_mix(
		vec3(0.002f, 0.022f, 0.060f), vec3(0.005f, 0.09f, 0.18f),
		renderer->frame->environment.daylight);
	struct armada_vec3 base =
		color_mix(near_water, deep_water, absorption * 0.82f);
	float crest = smoothstep(
		f_clamp((sample.height - ARMADA_WATER_Y + 0.008f) / 0.070f,
			0.0f, 1.0f));
	float steepness = smoothstep(f_clamp(
		(f_abs(sample.gradient_x) + f_abs(sample.gradient_z) - 0.018f) /
			0.075f,
		0.0f, 1.0f));
	struct armada_vec3 crest_color =
		color_mix(vec3(0.025f, 0.20f, 0.29f), vec3(0.16f, 0.58f, 0.62f),
			  renderer->frame->environment.daylight);
	float surface_cloud = surface_cloud_coverage(renderer, point);
	struct armada_vec3 reflection =
		sky_color(renderer, point, reflected, light_transmission);
	struct armada_vec3 glint = vec_add(
		water_body_glint(
			reflected, renderer->frame->environment.sun_direction,
			renderer->frame->environment.sun_visibility,
			sun_body_color(renderer), 1.0f - surface_cloud, 0.30f),
		water_body_glint(
			reflected, renderer->frame->environment.moon_direction,
			renderer->frame->environment.moon_visibility,
			vec3(0.68f, 0.80f, 1.0f), 1.0f - surface_cloud, 0.16f));

	base = color_mix(
		base, crest_color,
		crest * steepness *
			(0.07f +
			 renderer->frame->environment.daylight * 0.07f));
	base = vec_scale(base, 1.0f - surface_cloud * 0.16f);
	/* Filter direct illumination, not the ambient sky reflected by the water. */
	base = color_filter(base, color_mix(vec3(1.0f, 1.0f, 1.0f),
					    light_transmission, 0.65f));
	glint = color_filter(glint, light_transmission);
	return vec_scale(vec_add(color_mix(base, reflection, fresnel), glint),
			 renderer->frame->environment.exposure);
}

static struct armada_vec3 output_ray(const struct armada_renderer *renderer,
				     unsigned int x, unsigned int y)
{
	float denominator = (float)renderer->height * 0.95f;

	return vec_normalize(vec3(
		((float)(2U * x + 1U) - (float)renderer->width) / denominator,
		((float)renderer->height * 1.125f - (float)(2U * y + 1U)) /
			denominator,
		1.0f));
}

static struct armada_vec3
shade_far_water(const struct armada_renderer *renderer,
		struct armada_vec3 direction, float distance,
		struct armada_water_sample sample,
		struct armada_vec3 light_transmission)
{
	struct armada_vec3 point =
		vec_add(camera_position(), vec_scale(direction, distance));
	struct armada_vec3 deep = color_mix(
		vec3(0.002f, 0.022f, 0.060f), vec3(0.005f, 0.09f, 0.18f),
		renderer->frame->environment.daylight);
	float haze = f_clamp((distance - ARMADA_NEAR_WATER_DISTANCE) /
				     (ARMADA_WATER_FAR_DISTANCE -
				      ARMADA_NEAR_WATER_DISTANCE),
			     0.0f, 1.0f);
	struct armada_vec3 normal =
		water_normal(renderer, point, sample, distance);
	struct armada_vec3 reflected = vec_reflect(direction, normal);
	struct armada_vec3 view = vec_scale(direction, -1.0f);
	struct armada_vec3 sky =
		sky_color(renderer, point, reflected, light_transmission);
	float grazing = 1.0f - f_clamp(vec_dot(normal, view), 0.0f, 1.0f);
	float squared = grazing * grazing;
	float fresnel = 0.040f + 0.960f * squared * squared * grazing;
	struct armada_vec3 base = color_mix(deep, sky, haze * 0.32f);
	float surface_cloud = surface_cloud_coverage(renderer, point);
	struct armada_vec3 glint = vec_add(
		water_body_glint(
			reflected, renderer->frame->environment.sun_direction,
			renderer->frame->environment.sun_visibility,
			sun_body_color(renderer), 1.0f - surface_cloud, 0.30f),
		water_body_glint(
			reflected, renderer->frame->environment.moon_direction,
			renderer->frame->environment.moon_visibility,
			vec3(0.68f, 0.80f, 1.0f), 1.0f - surface_cloud, 0.16f));

	base = color_filter(base, color_mix(vec3(1.0f, 1.0f, 1.0f),
					    light_transmission, 0.65f));
	glint = color_filter(glint, light_transmission);
	return vec_scale(vec_add(color_mix(base, sky, fresnel), glint),
			 renderer->frame->environment.exposure);
}

static struct armada_vec3
water_light_transmission(const struct armada_renderer *renderer,
			 struct armada_vec3 point);

static struct armada_vec3
triangle_background_sample(const struct armada_renderer *renderer,
			   struct armada_vec3 direction)
{
	struct armada_vec3 camera = camera_position();
	struct armada_vec3 point;
	struct armada_water_sample water;
	float distance;

	if (water_intersection(renderer, camera, direction, &distance)) {
		struct armada_vec3 transmission;

		point = vec_add(camera, vec_scale(direction, distance));
		water = water_sample(renderer, point.x, point.z);
		transmission = water_light_transmission(renderer, point);
		if (distance > ARMADA_NEAR_WATER_DISTANCE)
			return shade_far_water(renderer, direction, distance,
					       water, transmission);
		return triangle_background_water(renderer, point, direction,
						 distance, water, transmission);
	}
	return vec_scale(sky_color(renderer, camera, direction,
				   vec3(1.0f, 1.0f, 1.0f)),
			 renderer->frame->environment.exposure);
}

static void upscale_background(struct armada_renderer *renderer)
{
	unsigned int source_y;

	for (source_y = 0; source_y < renderer->low_height; ++source_y) {
		unsigned int next_y = source_y + 1U < renderer->low_height ?
					      source_y + 1U :
					      source_y;
		const uint16_t *row0 = renderer->low_pixels +
				       (size_t)source_y * renderer->low_width;
		const uint16_t *row1 = renderer->low_pixels +
				       (size_t)next_y * renderer->low_width;
		unsigned int fraction_y;

		for (fraction_y = 0; fraction_y < ARMADA_BACKGROUND_SCALE;
		     ++fraction_y) {
			unsigned int y =
				source_y * ARMADA_BACKGROUND_SCALE + fraction_y;
			unsigned int vertical0 =
				ARMADA_BACKGROUND_SCALE - fraction_y;
			unsigned int source_x;

			if (y >= renderer->height)
				break;
			for (source_x = 0; source_x < renderer->low_width;
			     ++source_x) {
				unsigned int next_x =
					source_x + 1U < renderer->low_width ?
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

					if (x >= renderer->width)
						break;
					red = left_red * horizontal0 +
					      right_red * fraction_x;
					green = left_green * horizontal0 +
						right_green * fraction_x;
					blue = left_blue * horizontal0 +
					       right_blue * fraction_x;
					renderer->pixels[(size_t)y *
								 renderer->width +
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

static void refine_celestial_body(struct armada_renderer *renderer,
				  struct armada_vec3 body_direction,
				  float visibility, float radius)
{
	float denominator = (float)renderer->height * 0.95f;
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
	center_x = ((float)renderer->width +
		    body_direction.x / body_direction.z * denominator - 1.0f) *
		   0.5f;
	center_y = ((float)renderer->height * 1.125f -
		    body_direction.y / body_direction.z * denominator - 1.0f) *
		   0.5f;
	minimum_x = (int)(center_x - (float)extent);
	maximum_x = (int)(center_x + (float)extent);
	minimum_y = (int)(center_y - (float)extent);
	maximum_y = (int)(center_y + (float)extent);
	if (maximum_x < 0 || maximum_y < 0 ||
	    minimum_x >= (int)renderer->width ||
	    minimum_y >= (int)renderer->height)
		return;
	minimum_x = minimum_x < 0 ? 0 : minimum_x;
	minimum_y = minimum_y < 0 ? 0 : minimum_y;
	maximum_x = maximum_x >= (int)renderer->width ?
			    (int)renderer->width - 1 :
			    maximum_x;
	maximum_y = maximum_y >= (int)renderer->height ?
			    (int)renderer->height - 1 :
			    maximum_y;

	for (y = minimum_y; y <= maximum_y; ++y) {
		int x;

		for (x = minimum_x; x <= maximum_x; ++x) {
			struct armada_vec3 direction = output_ray(
				renderer, (unsigned int)x, (unsigned int)y);
			struct armada_vec3 color;

			if (celestial_plane_distance_squared(direction,
							     body_direction) >
			    candidate_radius * candidate_radius)
				continue;
			color = triangle_background_sample(renderer, direction);
			renderer->pixels[(size_t)y * renderer->width +
					 (unsigned int)x] =
				color_to_rgb565(color);
		}
	}
}

static void refine_celestial_bodies(struct armada_renderer *renderer)
{
	refine_celestial_body(renderer,
			      renderer->frame->environment.sun_direction,
			      renderer->frame->environment.sun_visibility,
			      ARMADA_SUN_RADIUS);
	refine_celestial_body(renderer,
			      renderer->frame->environment.moon_direction,
			      renderer->frame->environment.moon_visibility,
			      ARMADA_MOON_RADIUS);
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
triangle_stroke_world(const struct armada_renderer *renderer,
		      unsigned int letter,
		      const struct armada_prepared_stroke *stroke, float along,
		      float across, float depth, bool reflection)
{
	const struct armada_letter_state *state =
		&renderer->frame->letters[letter];
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

static bool triangle_project(const struct armada_renderer *renderer,
			     struct triangle_vertex *vertex, bool reflection)
{
	struct armada_vec3 relative = vec_sub(vertex->world, camera_position());
	float scale = (float)renderer->height * 0.95f;

	if (relative.z <= 0.05f)
		return false;
	vertex->x = ((float)renderer->width + relative.x / relative.z * scale -
		     1.0f) *
		    0.5f;
	vertex->y = ((float)renderer->height * 1.125f -
		     relative.y / relative.z * scale - 1.0f) *
		    0.5f;
	if (reflection) {
		struct armada_water_sample water = water_sample(
			renderer, vertex->world.x, vertex->world.z);

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
prepare_glass_surface(const struct armada_renderer *renderer,
		      unsigned int letter, struct armada_vec3 normal,
		      struct armada_vec3 point, bool reflection)
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
	float thickness = 0.34f * renderer->frame->letters[letter].scale;
	struct armada_vec3 exit_point = vec_add(
		relative, vec_scale(through, thickness / transmitted_cosine));
	float focal = (float)renderer->height * 0.475f;
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
	half_vector = vec_normalize(vec_add(
		renderer->frame->environment.celestial_direction, view));
	diffuse =
		f_max(vec_dot(normal,
			      renderer->frame->environment.celestial_direction),
		      0.0f);
	highlight = f_max(vec_dot(normal, half_vector), 0.0f);
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	environment = reflected.y > 0.0f ?
			      sky_color(renderer, point, reflected,
					vec3(1.0f, 1.0f, 1.0f)) :
			      color_mix(vec3(0.008f, 0.045f, 0.075f),
					vec3(0.055f, 0.24f, 0.30f),
					renderer->frame->environment.daylight);
	/* A little colored scattering keeps the small stained-glass glyphs readable. */
	scatter = (0.80f + 0.18f * renderer->frame->environment.daylight +
		   diffuse * 0.12f) *
		  (1.0f - reflectance);
	glass.light = vec_add(vec_scale(environment, reflectance),
			      vec_scale(base, scatter));
	glass.light = vec_add(
		glass.light,
		vec_scale(base,
			  rim_squared *
				  (0.18f + renderer->frame->environment.night *
						   0.15f)));
	glass.light = vec_add(
		glass.light,
		vec_scale(renderer->frame->environment.light_color,
			  highlight * 0.38f * renderer->cloud_transmittance));
	glass.light = vec_add(
		glass.light,
		vec_scale(color_mix(base, vec3(1.0f, 1.0f, 1.0f), 0.65f),
			  renderer->frame->letters[letter].emissive *
				  (0.26f + rim * 0.55f)));
	glass.light =
		vec_scale(glass.light, renderer->frame->environment.exposure);
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

static struct armada_vec3 glass_pixel(const struct armada_renderer *renderer,
				      const struct armada_glass_surface *glass,
				      unsigned int x, unsigned int y)
{
	float sample_x = f_clamp((float)x + glass->offset_x, 0.0f,
				 (float)renderer->width - 1.0f);
	float sample_y = f_clamp((float)y + glass->offset_y, 0.0f,
				 (float)renderer->height - 1.0f);
	unsigned int left = (unsigned int)sample_x;
	unsigned int top = (unsigned int)sample_y;
	unsigned int right = left + (left + 1U < renderer->width);
	unsigned int bottom = top + (top + 1U < renderer->height);
	float fraction_x = sample_x - (float)left;
	float fraction_y = sample_y - (float)top;
	struct armada_vec3 upper = color_mix(
		rgb565_color(renderer->background_pixels
				     [(size_t)top * renderer->width + left]),
		rgb565_color(renderer->background_pixels
				     [(size_t)top * renderer->width + right]),
		fraction_x);
	struct armada_vec3 lower = color_mix(
		rgb565_color(renderer->background_pixels
				     [(size_t)bottom * renderer->width + left]),
		rgb565_color(
			renderer->background_pixels
				[(size_t)bottom * renderer->width + right]),
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

static void shadow_rasterize_triangle(const struct armada_renderer *renderer,
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
	float first_depth = vec_dot(first->world, renderer->shadow_direction);
	float second_depth = vec_dot(second->world, renderer->shadow_direction);
	float third_depth = vec_dot(third->world, renderer->shadow_direction);
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

static void prepare_shadow_light_samples(struct armada_renderer *renderer,
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
	struct armada_vec3 center =
		vec_scale(renderer->shadow_direction,
			  1.0f / renderer->shadow_direction.z);
	unsigned int sample;

	renderer->shadow_max_slope_u = 0.0f;
	renderer->shadow_max_slope_v = 0.0f;
	for (sample = 0; sample < ARMADA_LIGHT_SAMPLES; ++sample) {
		/* Use the same projected disk as the sky, not a radius in guessed radians. */
		struct armada_vec3 direction =
			vec_add(center, vec3(disk[sample][0] * radius,
					     disk[sample][1] * radius, 0.0f));
		float forward = vec_dot(direction, renderer->shadow_direction);
		float u = vec_dot(direction, renderer->shadow_right) / forward;
		float v = vec_dot(direction, renderer->shadow_up) / forward;

		renderer->shadow_light_slopes[sample][0] = u;
		renderer->shadow_light_slopes[sample][1] = v;
		renderer->shadow_max_slope_u =
			f_max(renderer->shadow_max_slope_u, f_abs(u));
		renderer->shadow_max_slope_v =
			f_max(renderer->shadow_max_slope_v, f_abs(v));
	}
}

static void render_water_shadows(struct armada_renderer *renderer)
{
	struct triangle_vertex vertices[ARMADA_STROKES][12];
	bool sunlight = renderer->frame->environment.sun_visibility >=
			renderer->frame->environment.moon_visibility;
	float minimum_u = FLT_MAX;
	float maximum_u = -FLT_MAX;
	float minimum_v = FLT_MAX;
	float maximum_v = -FLT_MAX;
	unsigned int letter;
	size_t index;

	renderer->shadow_direction =
		sunlight ? renderer->frame->environment.sun_direction :
			   renderer->frame->environment.moon_direction;
	renderer->shadow_strength =
		(sunlight ?
			 renderer->frame->environment.sun_visibility *
				 (0.48f +
				  renderer->frame->environment.daylight *
					  0.44f) :
			 renderer->frame->environment.moon_visibility *
				 renderer->frame->environment.night * 0.28f) *
		smoothstep(renderer->shadow_direction.y / 0.18f) *
		renderer->cloud_transmittance;
	if (renderer->shadow_strength <= 0.0f)
		return;
	/* Orthographic light coordinates: no camera position or point-light falloff. */
	renderer->shadow_right =
		vec_normalize(vec3(renderer->shadow_direction.z, 0.0f,
				   -renderer->shadow_direction.x));
	renderer->shadow_up = triangle_cross(renderer->shadow_direction,
					     renderer->shadow_right);
	prepare_shadow_light_samples(renderer, sunlight);
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		struct armada_shadow_bounds *bounds =
			&renderer->shadow_bounds[letter];
		unsigned int stroke;

		*bounds = (struct armada_shadow_bounds){
			.minimum_u = FLT_MAX,
			.maximum_u = -FLT_MAX,
			.minimum_v = FLT_MAX,
			.maximum_v = -FLT_MAX,
			.maximum_depth = -FLT_MAX,
		};
		renderer->shadow_filter[letter] =
			vec_scale(color_mix(vec3(1.0f, 1.0f, 1.0f),
					    letter_base_color(letter),
					    ARMADA_GLASS_ABSORPTION),
				  0.92f);
		if (!renderer->frame->letters[letter].visible)
			continue;
		for (stroke = 0; stroke < glyph->count; ++stroke) {
			unsigned int stroke_index = glyph->first + stroke;
			const struct armada_prepared_stroke *prepared =
				&renderer->prepared_strokes[stroke_index];
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
					renderer, letter, prepared, along,
					profile->across, profile->depth, false);
				vertex->x = vec_dot(vertex->world,
						    renderer->shadow_right);
				vertex->y = vec_dot(vertex->world,
						    renderer->shadow_up);
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
				bounds->maximum_depth = f_max(
					bounds->maximum_depth,
					vec_dot(vertex->world,
						renderer->shadow_direction));
			}
		}
	}
	if (minimum_u == FLT_MAX) {
		renderer->shadow_strength = 0.0f;
		return;
	}
	renderer->shadow_min_u = minimum_u - 0.12f;
	renderer->shadow_min_v = minimum_v - 0.12f;
	renderer->shadow_scale_u = (float)(ARMADA_SHADOW_SIDE - 1U) /
				   (maximum_u - minimum_u + 0.24f);
	renderer->shadow_scale_v = (float)(ARMADA_SHADOW_SIDE - 1U) /
				   (maximum_v - minimum_v + 0.24f);
	for (index = 0; index < (size_t)ARMADA_LETTERS * ARMADA_SHADOW_SIDE *
					ARMADA_SHADOW_SIDE;
	     ++index)
		renderer->shadow_depth[index] = -FLT_MAX;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		float *depth = renderer->shadow_depth +
			       (size_t)letter * ARMADA_SHADOW_SIDE *
				       ARMADA_SHADOW_SIDE;
		unsigned int stroke;

		if (!renderer->frame->letters[letter].visible)
			continue;
		{
			struct armada_shadow_bounds *bounds =
				&renderer->shadow_bounds[letter];

			bounds->minimum_u =
				(bounds->minimum_u - renderer->shadow_min_u) *
					renderer->shadow_scale_u -
				0.5f;
			bounds->maximum_u =
				(bounds->maximum_u - renderer->shadow_min_u) *
					renderer->shadow_scale_u -
				0.5f;
			bounds->minimum_v =
				(bounds->minimum_v - renderer->shadow_min_v) *
					renderer->shadow_scale_v -
				0.5f;
			bounds->maximum_v =
				(bounds->maximum_v - renderer->shadow_min_v) *
					renderer->shadow_scale_v -
				0.5f;
		}
		for (stroke = 0; stroke < glyph->count; ++stroke) {
			struct triangle_vertex *projected =
				vertices[glyph->first + stroke];
			unsigned int corner;
			unsigned int face;

			for (corner = 0; corner < 12U; ++corner) {
				projected[corner].x = (projected[corner].x -
						       renderer->shadow_min_u) *
						      renderer->shadow_scale_u;
				projected[corner].y = (projected[corner].y -
						       renderer->shadow_min_v) *
						      renderer->shadow_scale_v;
			}
			for (face = 0; face < ARRAY_SIZE(stroke_prism_faces);
			     ++face)
				shadow_rasterize_triangle(
					renderer, depth,
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

static float shadow_letter_coverage(const struct armada_renderer *renderer,
				    unsigned int letter, float u, float v,
				    float receiver_depth)
{
	const float *depth =
		renderer->shadow_depth +
		(size_t)letter * ARMADA_SHADOW_SIDE * ARMADA_SHADOW_SIDE;
	const struct armada_shadow_bounds *bounds =
		&renderer->shadow_bounds[letter];
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
	radius_u = maximum_separation * renderer->shadow_max_slope_u *
		   renderer->shadow_scale_u;
	radius_v = maximum_separation * renderer->shadow_max_slope_v *
		   renderer->shadow_scale_v;
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
					renderer->shadow_light_slopes[sample][0] *
					renderer->shadow_scale_u,
			v + maximum_separation *
					renderer->shadow_light_slopes[sample][1] *
					renderer->shadow_scale_v,
			receiver_depth);

		blocker_weight += blocker.coverage;
		blocker_separation += blocker.separation;
	}
	if (blocker_weight <= 0.0f)
		return 0.0f;
	separation = blocker_separation / blocker_weight;
	for (sample = 0; sample < ARMADA_LIGHT_SAMPLES; ++sample)
		coverage +=
			shadow_sample(
				depth,
				u + separation *
						renderer->shadow_light_slopes
							[sample][0] *
						renderer->shadow_scale_u,
				v + separation *
						renderer->shadow_light_slopes
							[sample][1] *
						renderer->shadow_scale_v,
				receiver_depth)
				.coverage;
	return coverage / (float)ARMADA_LIGHT_SAMPLES;
}

static struct armada_vec3
water_light_transmission(const struct armada_renderer *renderer,
			 struct armada_vec3 point)
{
	struct armada_vec3 transmission = vec3(1.0f, 1.0f, 1.0f);
	float u;
	float v;
	float receiver_depth;
	unsigned int letter;

	if (renderer->shadow_strength <= 0.0f)
		return transmission;
	u = (vec_dot(point, renderer->shadow_right) - renderer->shadow_min_u) *
		    renderer->shadow_scale_u -
	    0.5f;
	v = (vec_dot(point, renderer->shadow_up) - renderer->shadow_min_v) *
		    renderer->shadow_scale_v -
	    0.5f;
	/* Compare at the actual wavy-water hit, so submerged casters cannot shade it. */
	receiver_depth = vec_dot(point, renderer->shadow_direction) + 0.005f;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		float coverage = shadow_letter_coverage(renderer, letter, u, v,
							receiver_depth);

		/* One filter per letter, not per overlapping triangle or stroke. */
		transmission = color_filter(
			transmission,
			color_mix(vec3(1.0f, 1.0f, 1.0f),
				  renderer->shadow_filter[letter],
				  coverage * renderer->shadow_strength));
	}
	return transmission;
}

static bool triangle_water_at_pixel(const struct armada_renderer *renderer,
				    unsigned int x, unsigned int y,
				    float *surface_z)
{
	size_t index = (size_t)y * renderer->width + x;
	float cached = renderer->water_surface_z[index];
	struct armada_vec3 direction;
	float distance;

	if (cached != 0.0f) {
		if (cached < 0.0f)
			return false;
		*surface_z = cached;
		return true;
	}
	direction = renderer->full_rays[index];
	if (!water_intersection(renderer, camera_position(), direction,
				&distance)) {
		renderer->water_surface_z[index] = -1.0f;
		return false;
	}
	*surface_z = distance * direction.z;
	renderer->water_surface_z[index] = *surface_z;
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

static void triangle_rasterize(struct armada_renderer *renderer,
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
		reflection_visibility = letter_reflection_visibility(
			&renderer->frame->letters[letter]);
		if (reflection_visibility <= 0.0f)
			return;
	}
	minimum_x = f_min(first->x, f_min(second->x, third->x));
	maximum_x = f_max(first->x, f_max(second->x, third->x));
	minimum_y = f_min(first->y, f_min(second->y, third->y));
	maximum_y = f_max(first->y, f_max(second->y, third->y));
	if (maximum_x < 0.0f || maximum_y < 0.0f ||
	    minimum_x >= (float)renderer->width ||
	    minimum_y >= (float)renderer->height)
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
	glass = prepare_glass_surface(renderer, letter, normal, midpoint,
				      reflection);
	first_x = (int)f_max(minimum_x, 0.0f);
	last_x = (int)f_min(maximum_x, (float)renderer->width - 1.0f);
	first_y = (int)f_max(minimum_y, 0.0f);
	last_y = (int)f_min(maximum_y, (float)renderer->height - 1.0f);
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
							renderer->width +
						(unsigned int)x;
					if (reflection) {
						float unused_surface_z;

						if (inverse_z <=
							    renderer->reflection_depth
								    [index] ||
						    !triangle_water_at_pixel(
							    renderer,
							    (unsigned int)x,
							    (unsigned int)y,
							    &unused_surface_z))
							continue;
						renderer->reflection_depth[index] =
							inverse_z;
						renderer->pixels
							[index] = triangle_mix_rgb565(
							renderer->pixels[index],
							vec_scale(
								glass_pixel(
									renderer,
									&glass,
									(unsigned int)
										x,
									(unsigned int)
										y),
								0.58f),
							(0.10f +
							 0.24f * renderer->frame
									 ->environment
									 .daylight) *
								reflection_visibility);
					} else {
						float surface_z;

						if (inverse_z <=
						    renderer->triangle_depth
							    [index])
							continue;
						if (triangle_water_at_pixel(
							    renderer,
							    (unsigned int)x,
							    (unsigned int)y,
							    &surface_z) &&
						    1.0f / inverse_z >
							    surface_z)
							continue;
						renderer->triangle_depth[index] =
							inverse_z;
						renderer->pixels[index] =
							color_to_rgb565(glass_pixel(
								renderer,
								&glass,
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
triangle_rasterize_stroke(struct armada_renderer *renderer, unsigned int letter,
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
			renderer, letter, stroke,
			along[vertex / ARRAY_SIZE(stroke_prism_profile)],
			profile->across, profile->depth, reflection);
		if (!triangle_project(renderer, &vertices[vertex], reflection))
			return;
	}
	for (face = 0; face < ARRAY_SIZE(stroke_prism_faces); ++face)
		triangle_rasterize(renderer,
				   &vertices[stroke_prism_faces[face][0]],
				   &vertices[stroke_prism_faces[face][1]],
				   &vertices[stroke_prism_faces[face][2]],
				   letter, reflection);
}

static void triangle_rasterize_letters(struct armada_renderer *renderer,
				       bool reflection)
{
	unsigned int letter;

	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_glyph *glyph = &glyphs[letter];
		unsigned int index;

		if (!renderer->frame->letters[letter].visible)
			continue;
		if (reflection &&
		    letter_reflection_visibility(
			    &renderer->frame->letters[letter]) <= 0.0f)
			continue;
		for (index = 0; index < glyph->count; ++index)
			triangle_rasterize_stroke(
				renderer, letter,
				&renderer->prepared_strokes[glyph->first +
							    index],
				reflection);
	}
}

static void render_scene(struct armada_renderer *renderer)
{
	unsigned int y;

	memset(renderer->triangle_depth, 0,
	       (size_t)renderer->width * renderer->height *
		       sizeof(*renderer->triangle_depth));
	memset(renderer->reflection_depth, 0,
	       (size_t)renderer->width * renderer->height *
		       sizeof(*renderer->reflection_depth));
	memset(renderer->water_surface_z, 0,
	       (size_t)renderer->width * renderer->height *
		       sizeof(*renderer->water_surface_z));
	render_water_shadows(renderer);
	for (y = 0; y < renderer->low_height; ++y) {
		unsigned int x;

		for (x = 0; x < renderer->low_width; ++x) {
			struct armada_vec3 color = triangle_background_sample(
				renderer,
				renderer->rays[(size_t)y * renderer->low_width +
					       x]);

			renderer->low_pixels[(size_t)y * renderer->low_width +
					     x] = color_to_rgb565(color);
		}
	}
	upscale_background(renderer);
	refine_celestial_bodies(renderer);
	/* Each glass pass reads a complete current-frame background, never itself. */
	memcpy(renderer->background_pixels, renderer->pixels,
	       (size_t)renderer->width * renderer->height *
		       sizeof(*renderer->pixels));
	triangle_rasterize_letters(renderer, true);
	memcpy(renderer->background_pixels, renderer->pixels,
	       (size_t)renderer->width * renderer->height *
		       sizeof(*renderer->pixels));
	triangle_rasterize_letters(renderer, false);
}

static void put_pixel(struct armada_renderer *renderer, int x, int y,
		      uint16_t color)
{
	if (x >= 0 && y >= 0 && (unsigned int)x < renderer->width &&
	    (unsigned int)y < renderer->height)
		renderer->pixels[(size_t)y * renderer->width + (unsigned int)x] =
			color;
}

static void draw_character(struct armada_renderer *renderer, int x, int y,
			   char character, uint16_t color)
{
	const struct fplinux_font *font = renderer->font;
	const unsigned char *glyph;

	if (character >= 'a' && character <= 'z')
		character = (char)(character - 'a' + 'A');
	glyph = fplinux_font_glyph(font, (unsigned char)character);
	if (!glyph)
		return;
	for (unsigned int row = 0; row < font->height; ++row)
		for (unsigned int column = 0; column < font->width; ++column)
			if (glyph[row * font->row_bytes + column / 8U] &
			    (0x80U >> (column % 8U)))
				put_pixel(renderer, x + (int)column,
					  y + (int)row, color);
}

static void draw_text(struct armada_renderer *renderer, int x, int y,
		      const char *text, uint16_t color)
{
	while (*text) {
		draw_character(renderer, x, y, *text, color);
		x += (int)renderer->font->width;
		++text;
	}
}

static void draw_centered(struct armada_renderer *renderer, int y,
			  const char *text, uint16_t color)
{
	int width = (int)(strlen(text) * renderer->font->width);

	draw_text(renderer, ((int)renderer->width - width) / 2, y, text, color);
}

static void draw_overlay(struct armada_renderer *renderer,
			 const struct armada_metrics *metrics)
{
	unsigned int line_height = renderer->font->height + 2U;
	uint16_t cyan = color_to_rgb565(vec3(0.25f, 0.88f, 1.0f));
	uint16_t white = color_to_rgb565(vec3(0.90f, 0.96f, 1.0f));
	char line[48];

	draw_centered(renderer, (int)renderer->height / 12, "FPLINUX ARMADA",
		      white);
	if (renderer->frame->number >= ARMADA_FINAL_METRICS && metrics &&
	    metrics->valid) {
		snprintf(line, sizeof(line), "AVG %u.%u FPS",
			 metrics->average_fps_tenths / 10U,
			 metrics->average_fps_tenths % 10U);
		draw_centered(renderer, (int)renderer->height * 3 / 4, line,
			      cyan);
		snprintf(line, sizeof(line), "MIN %u.%u  MAX %u MS",
			 metrics->minimum_fps_tenths / 10U,
			 metrics->minimum_fps_tenths % 10U,
			 metrics->maximum_frame_us / 1000U);
		draw_centered(renderer,
			      (int)renderer->height * 3 / 4 + (int)line_height,
			      line, white);
		draw_centered(renderer,
			      (int)renderer->height - (int)line_height,
			      "BACK EXITS", white);
	} else {
		draw_centered(renderer,
			      (int)renderer->height - (int)line_height,
			      "DISPLAY KEYS HAPTICS", cyan);
	}
}

struct armada_renderer *armada_renderer_create(unsigned int width,
					       unsigned int height,
					       uint16_t *pixels,
					       const struct fplinux_font *font)
{
	struct armada_renderer *renderer;
	unsigned int y;

	renderer = calloc(1, sizeof(*renderer));
	if (!renderer)
		return NULL;
	renderer->font = font;
	renderer->width = width;
	renderer->height = height;
	renderer->low_width = (width + ARMADA_BACKGROUND_SCALE - 1U) /
			      ARMADA_BACKGROUND_SCALE;
	renderer->low_height = (height + ARMADA_BACKGROUND_SCALE - 1U) /
			       ARMADA_BACKGROUND_SCALE;
	renderer->pixels = pixels;
	renderer->background_pixels = calloc(
		(size_t)width * height, sizeof(*renderer->background_pixels));
	renderer->low_pixels =
		calloc((size_t)renderer->low_width * renderer->low_height,
		       sizeof(*renderer->low_pixels));
	renderer->shadow_depth =
		calloc((size_t)ARMADA_LETTERS * ARMADA_SHADOW_SIDE *
			       ARMADA_SHADOW_SIDE,
		       sizeof(*renderer->shadow_depth));
	renderer->rays =
		calloc((size_t)renderer->low_width * renderer->low_height,
		       sizeof(*renderer->rays));
	renderer->full_rays =
		calloc((size_t)width * height, sizeof(*renderer->full_rays));
	renderer->triangle_depth = calloc((size_t)width * height,
					  sizeof(*renderer->triangle_depth));
	renderer->reflection_depth = calloc(
		(size_t)width * height, sizeof(*renderer->reflection_depth));
	renderer->water_surface_z = calloc((size_t)width * height,
					   sizeof(*renderer->water_surface_z));
	if (!renderer->background_pixels || !renderer->low_pixels ||
	    !renderer->shadow_depth || !renderer->rays ||
	    !renderer->full_rays || !renderer->triangle_depth ||
	    !renderer->reflection_depth || !renderer->water_surface_z) {
		free(renderer->water_surface_z);
		free(renderer->reflection_depth);
		free(renderer->triangle_depth);
		free(renderer->full_rays);
		free(renderer->rays);
		free(renderer->shadow_depth);
		free(renderer->low_pixels);
		free(renderer->background_pixels);
		free(renderer);
		return NULL;
	}
	prepare_stroke_geometry(renderer);
	for (y = 0; y < renderer->height; ++y) {
		unsigned int x;

		for (x = 0; x < renderer->width; ++x)
			renderer->full_rays[(size_t)y * renderer->width + x] =
				output_ray(renderer, x, y);
	}
	for (y = 0; y < renderer->low_height; ++y) {
		unsigned int x;

		for (x = 0; x < renderer->low_width; ++x) {
			float denominator = (float)renderer->low_height * 0.95f;
			struct armada_vec3 direction =
				vec3(((float)(2U * x + 1U) -
				      (float)renderer->low_width) /
					     denominator,
				     ((float)renderer->low_height * 1.125f -
				      (float)(2U * y + 1U)) /
					     denominator,
				     1.0f);

			renderer->rays[(size_t)y * renderer->low_width + x] =
				vec_normalize(direction);
		}
	}
	return renderer;
}

void armada_renderer_destroy(struct armada_renderer *renderer)
{
	if (!renderer)
		return;
	free(renderer->water_surface_z);
	free(renderer->reflection_depth);
	free(renderer->triangle_depth);
	free(renderer->full_rays);
	free(renderer->rays);
	free(renderer->shadow_depth);
	free(renderer->low_pixels);
	free(renderer->background_pixels);
	free(renderer);
}

void armada_renderer_render(struct armada_renderer *renderer,
			    const struct armada_frame *frame,
			    const struct armada_metrics *metrics)
{
	renderer->frame = frame;
	renderer->cloud_transmittance =
		1.0f -
		cloud_coverage_ray(renderer, vec3(0.0f, 0.75f, -1.35f),
				   frame->environment.celestial_direction) *
			0.20f;
	render_scene(renderer);
	draw_overlay(renderer, metrics);
}
