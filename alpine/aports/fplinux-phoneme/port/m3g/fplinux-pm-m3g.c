// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#include <kni.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sni.h>

enum {
	X,
	Y,
	Z,
	W,
	U,
	V,
	A,
	R,
	G,
	B,
	BACK_R,
	BACK_G,
	BACK_B,
	FOG,
	VERTEX_COMPONENTS,
};

enum {
	VIEW_WIDTH,
	VIEW_HEIGHT,
	VIEW_X,
	VIEW_Y,
	VIEWPORT_WIDTH,
	VIEWPORT_HEIGHT,
	/* Bits of the floats that depth 0 and 1 map to. */
	VIEW_DEPTH_NEAR,
	VIEW_DEPTH_FAR,
	VIEW_FIELDS,
};

enum {
	CULLING,
	WINDING,
	FLAGS,
	BLEND,
	THRESHOLD,
	DEPTH_UNITS,
	TEXTURE_WIDTH,
	TEXTURE_HEIGHT,
	WRAP_S,
	WRAP_T,
	TEXTURE_FUNCTION,
	BLEND_COLOR,
	DEPTH_FACTOR,
	TEXTURE_FORMAT,
	IMAGE_FILTER,
	FOG_COLOR,
	STATE_FIELDS,
};

enum {
	SMOOTH = 1,
	PERSPECTIVE = 2,
	DEPTH_TEST = 4,
	DEPTH_WRITE = 8,
	COLOR_WRITE = 16,
	ALPHA_WRITE = 32,
	BACK_LIGHTING = 64,
	FOG_ENABLED = 128,
	CULL_BACK = 160,
	CULL_FRONT = 161,
	WINDING_CCW = 168,
	TEXTURE_ADD = 224,
	TEXTURE_BLEND = 225,
	TEXTURE_DECAL = 226,
	TEXTURE_REPLACE = 228,
	WRAP_REPEAT = 241,
	IMAGE_ALPHA = 96,
	IMAGE_LUMINANCE = 97,
	IMAGE_RGB = 99,
	FILTER_LINEAR = 209,
	BACKGROUND_REPEAT = 33,
	ALPHA = 64,
	ALPHA_ADD = 65,
	MODULATE_X2 = 67,
	REPLACE = 68,
};

struct rectangle {
	int left, top, right, bottom;
};

struct target {
	jint *pixels;
	jfloat *depths;
	struct rectangle clip;
	int stride;
	float x, y, half_width, half_height;
	float depth_near, depth_scale;
};

struct surface {
	const jint *texture;
	const jint *state;
};

struct vertex {
	float values[VERTEX_COMPONENTS];
};

struct projected {
	float x, y, z, inv_w;
};

static int lower_bound(float value, int low, int high)
{
	if (value <= low)
		return low;
	if (value >= high)
		return high;
	return (int)value;
}

static int upper_bound(float value, int low, int high)
{
	int base = lower_bound(value, low, high);

	return base < high && base < value ? base + 1 : base;
}

/* The depth buffer is native memory so that it does not take Java heap. */
static jfloat *depth_buffer;
static jint depth_capacity;

static float state_float(jint value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

static bool target_init(struct target *target, jarray pixels, jarray view)
{
	if (KNI_IsNullHandle(pixels) || KNI_IsNullHandle(view) ||
	    KNI_GetArrayLength(view) != VIEW_FIELDS)
		return false;

	const jint *bounds = SNI_GetRawArrayPointer(view);
	int width = bounds[VIEW_WIDTH], height = bounds[VIEW_HEIGHT];
	int x = bounds[VIEW_X], y = bounds[VIEW_Y];
	int viewport_width = bounds[VIEWPORT_WIDTH];
	int viewport_height = bounds[VIEWPORT_HEIGHT];
	int64_t count = (int64_t)width * height;
	int64_t right = (int64_t)x + viewport_width;
	int64_t bottom = (int64_t)y + viewport_height;
	float depth_near = state_float(bounds[VIEW_DEPTH_NEAR]);
	float depth_far = state_float(bounds[VIEW_DEPTH_FAR]);

	if (width < 0 || height < 0 || viewport_width <= 0 ||
	    viewport_height <= 0 || count > KNI_GetArrayLength(pixels) ||
	    count > depth_capacity)
		return false;

	*target = (struct target){
		.pixels = SNI_GetRawArrayPointer(pixels),
		.depths = depth_buffer,
		.clip = {
			.left = x > 0 ? x : 0,
			.top = y > 0 ? y : 0,
			.right = right < width ? (int)right : width,
			.bottom = bottom < height ? (int)bottom : height,
		},
		.stride = width,
		.x = (float)x,
		.y = (float)y,
		.half_width = (float)viewport_width * .5f,
		.half_height = (float)viewport_height * .5f,
		.depth_near = depth_near,
		.depth_scale = depth_far - depth_near,
	};
	return true;
}

static bool empty(const struct rectangle *clip)
{
	return clip->left >= clip->right || clip->top >= clip->bottom;
}

static bool contains(const struct rectangle *outer,
		     const struct rectangle *inner)
{
	return outer->left <= inner->left && outer->top <= inner->top &&
	       outer->right >= inner->right && outer->bottom >= inner->bottom;
}

/*
 * Part of the depth buffer, with this row stride, that holds the cleared
 * depth because nothing was drawn since it was cleared. Games clear the depth
 * several times a frame, often twice in a row.
 */
static struct rectangle depth_cleared;
static int depth_cleared_stride;

/* Fills count 32-bit values: doubling copies instead of a store per value. */
static void fill32(void *start, const void *value, size_t count)
{
	unsigned char *bytes = start;
	size_t filled = 4, total = count * 4;

	if (count == 0)
		return;
	memcpy(bytes, value, 4);
	while (filled < total) {
		size_t chunk = filled < total - filled ? filled :
							 total - filled;

		memcpy(bytes + filled, bytes, chunk);
		filled += chunk;
	}
}

static void fill_rectangle(void *buffer, int stride,
			   const struct rectangle *clip, const void *value)
{
	size_t width = (size_t)(clip->right - clip->left);
	unsigned char *first = (unsigned char *)buffer +
			       ((size_t)clip->top * stride + clip->left) * 4;

	if (width == (size_t)stride) {
		fill32(first, value,
		       width * (size_t)(clip->bottom - clip->top));
		return;
	}
	fill32(first, value, width);
	for (int y = clip->top + 1; y < clip->bottom; ++y)
		memcpy(first + (size_t)(y - clip->top) * stride * 4, first,
		       width * 4);
}

static unsigned int channel(uint32_t color, unsigned int shift)
{
	return (color >> shift) & 255;
}

static uint32_t color_component(float value)
{
	if (!(value > 0))
		return 0;
	if (value >= 255)
		return 255;
	return (uint32_t)(value + .5f);
}

/*
 * color_component without the range tests. Precondition: value is finite and
 * |value| < 2^30, so that the conversion to an integer is defined.
 * rasterize() establishes this for the colors it interpolates.
 */
static uint32_t color_component_bounded(float value)
{
	int rounded = (int)(value + .5f);

	if (rounded < 0)
		return 0;
	return rounded > 255 ? 255 : (uint32_t)rounded;
}

static uint32_t argb(float alpha, const float *rgb)
{
	return color_component(alpha) << 24 | color_component(rgb[0]) << 16 |
	       color_component(rgb[1]) << 8 | color_component(rgb[2]);
}

static unsigned int saturated(unsigned int value)
{
	return value < 255 ? value : 255;
}

static unsigned int mix(unsigned int from, unsigned int to, unsigned int weight)
{
	return (from * (255 - weight) + to * weight + 127) / 255;
}

static uint32_t fog_color(uint32_t color, uint32_t fog, float visibility)
{
	unsigned int weight = color_component(visibility * 255);
	uint32_t result = color & 0xff000000U;

	for (unsigned int shift = 0; shift < 24; shift += 8)
		result |=
			mix(channel(fog, shift), channel(color, shift), weight)
			<< shift;
	return result;
}

static int texture_coordinate(float coord, int extent, int mode)
{
	if (!isfinite(coord))
		return 0;
	if (mode == WRAP_REPEAT)
		coord -= floorf(coord);
	return lower_bound(coord * extent, 0, extent - 1);
}

static int adjacent(int value, int extent, int mode)
{
	if (value < 0)
		return mode == WRAP_REPEAT ? extent - 1 : 0;
	if (value == extent)
		return mode == WRAP_REPEAT ? 0 : extent - 1;
	return value;
}

static float sample_position(float value, int extent, int mode)
{
	if (!isfinite(value))
		value = 0;
	if (mode == WRAP_REPEAT)
		value -= floorf(value);
	value = fminf(1, fmaxf(0, value));
	return value * extent - .5f;
}

static uint32_t sample_linear(const struct surface *surface, float u, float v)
{
	const jint *state = surface->state;
	int width = state[TEXTURE_WIDTH], height = state[TEXTURE_HEIGHT];
	int mode_s = state[WRAP_S], mode_t = state[WRAP_T];
	float x = sample_position(u, width, mode_s);
	float y = sample_position(v, height, mode_t);
	int sx = (int)floorf(x), sy = (int)floorf(y);
	int left = adjacent(sx, width, mode_s);
	int right = adjacent(sx + 1, width, mode_s);
	int top = adjacent(sy, height, mode_t) * width;
	int bottom = adjacent(sy + 1, height, mode_t) * width;
	unsigned int fx = (unsigned int)((x - sx) * 255 + .5f);
	unsigned int fy = (unsigned int)((y - sy) * 255 + .5f);
	uint32_t value = 0;

	for (unsigned int shift = 0; shift <= 24; shift += 8) {
		unsigned int a =
			mix(channel(surface->texture[top + left], shift),
			    channel(surface->texture[top + right], shift), fx);
		unsigned int b = mix(
			channel(surface->texture[bottom + left], shift),
			channel(surface->texture[bottom + right], shift), fx);

		value |= mix(a, b, fy) << shift;
	}
	return value;
}

static uint32_t texture_color(uint32_t color, const struct surface *surface,
			      float u, float v)
{
	const jint *state = surface->state;
	int width = state[TEXTURE_WIDTH];
	uint32_t texel;

	if (state[IMAGE_FILTER] == FILTER_LINEAR) {
		texel = sample_linear(surface, u, v);
	} else {
		int s = texture_coordinate(u, width, state[WRAP_S]);
		int t = texture_coordinate(v, state[TEXTURE_HEIGHT],
					   state[WRAP_T]);

		texel = (uint32_t)surface->texture[t * width + s];
	}
	unsigned int alpha = channel(color, 24), tex_alpha = channel(texel, 24);
	int function = state[TEXTURE_FUNCTION];
	int format = state[TEXTURE_FORMAT];
	bool has_color = format != IMAGE_ALPHA;
	bool has_alpha = format != IMAGE_LUMINANCE && format != IMAGE_RGB;
	uint32_t rgb = 0;

	if (function == TEXTURE_REPLACE)
		return ((has_alpha ? texel : color) & 0xff000000U) |
		       ((has_color ? texel : color) & 0x00ffffffU);
	if (!has_color) {
		if (function != TEXTURE_DECAL)
			alpha = alpha * tex_alpha / 255;
		return (color & 0x00ffffffU) | (alpha << 24);
	}
	if (color == 0xffffffffU && function != TEXTURE_ADD &&
	    function != TEXTURE_BLEND && function != TEXTURE_DECAL)
		return texel;

	for (unsigned int shift = 0; shift < 24; shift += 8) {
		unsigned int from = channel(color, shift);
		unsigned int to = channel(texel, shift);
		unsigned int value;

		switch (function) {
		case TEXTURE_DECAL:
			value = mix(from, to, tex_alpha);
			break;
		case TEXTURE_ADD:
			value = saturated(from + to);
			break;
		case TEXTURE_BLEND:
			value = mix(from,
				    channel((uint32_t)state[BLEND_COLOR],
					    shift),
				    to);
			break;
		default:
			value = from * to / 255;
			break;
		}
		rgb |= value << shift;
	}
	if (has_alpha && function != TEXTURE_DECAL)
		alpha = alpha * tex_alpha / 255;
	return alpha << 24 | rgb;
}

/*
 * mix() of all four channels with one weight, two channels per 32-bit
 * value: the products stay below 2^16 in their halves, and x / 255 for such
 * a x is (x + 1 + (x >> 8)) >> 8.
 */
static uint32_t blend_alpha(uint32_t source, uint32_t destination,
			    unsigned int weight)
{
	uint32_t result = 0;

	for (unsigned int shift = 0; shift < 9; shift += 8) {
		uint32_t from = (destination >> shift) & 0x00ff00ffU;
		uint32_t to = (source >> shift) & 0x00ff00ffU;
		uint32_t sum =
			from * (255 - weight) + to * weight + 0x007f007fU;

		sum += 0x00010001U + ((sum >> 8) & 0x00ff00ffU);
		result |= ((sum >> 8) & 0x00ff00ffU) << shift;
	}
	return result;
}

static uint32_t compose(int mode, uint32_t source, uint32_t destination)
{
	uint32_t rgb = 0;
	unsigned int alpha = channel(source, 24);

	if (mode == REPLACE)
		return source;
	if (mode == ALPHA) {
		/* The weights 255 and 0 give the source and the destination. */
		if (alpha == 255)
			return source;
		if (alpha == 0)
			return destination;
		return blend_alpha(source, destination, alpha);
	}

	for (unsigned int shift = 0; shift <= 24; shift += 8) {
		unsigned int from = channel(destination, shift);
		unsigned int to = channel(source, shift);
		unsigned int value;

		if (mode == ALPHA_ADD)
			value = saturated(from + to * alpha / 255);
		else
			value = saturated(from * to *
					  (mode == MODULATE_X2 ? 2 : 1) / 255);
		rgb |= value << shift;
	}
	return rgb;
}

static struct projected project(const struct target *target,
				const struct vertex *vertex)
{
	const float *values = vertex->values;
	float inv_w = 1.0f / values[W];

	return (struct projected){
		.x = target->x + (values[X] * inv_w + 1) * target->half_width,
		.y = target->y + (1 - values[Y] * inv_w) * target->half_height,
		.z = target->depth_near +
		     (values[Z] * inv_w * .5f + .5f) * target->depth_scale,
		.inv_w = inv_w,
	};
}

static float edge(const struct projected *a, const struct projected *b, float x,
		  float y)
{
	return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static bool includes_edge(const struct projected *a, const struct projected *b,
			  float area)
{
	float dx = (b->x - a->x) * area;
	float dy = (b->y - a->y) * area;

	return dy < 0 || (dy == 0 && dx > 0);
}

static void rasterize_reference(struct target *target,
				const struct surface *surface,
				const struct vertex *a, const struct vertex *b,
				const struct vertex *c,
				const struct vertex *flat)
{
	const jint *state = surface->state;
	const struct rectangle *clip = &target->clip;
	int flags = state[FLAGS];

	if (!(a->values[W] > 0 && b->values[W] > 0 && c->values[W] > 0))
		return;
	struct projected pa = project(target, a), pb = project(target, b);
	struct projected pc = project(target, c);
	float area = edge(&pa, &pb, pc.x, pc.y);
	bool front = state[WINDING] == WINDING_CCW ? area < 0 : area > 0;

	if (!isfinite(area) || area == 0 ||
	    (state[CULLING] == CULL_BACK && !front) ||
	    (state[CULLING] == CULL_FRONT && front))
		return;

	int left = lower_bound(fminf(pa.x, fminf(pb.x, pc.x)), clip->left,
			       clip->right);
	int top = lower_bound(fminf(pa.y, fminf(pb.y, pc.y)), clip->top,
			      clip->bottom);
	int right = upper_bound(fmaxf(pa.x, fmaxf(pb.x, pc.x)), clip->left,
				clip->right);
	int bottom = upper_bound(fmaxf(pa.y, fmaxf(pb.y, pc.y)), clip->top,
				 clip->bottom);
	if (left >= right || top >= bottom)
		return;

	float inv_area = 1.0f / area;
	float step_a = (pb.y - pc.y) * inv_area;
	float step_b = (pc.y - pa.y) * inv_area;
	float step_c = (pa.y - pb.y) * inv_area;
	float offset = state_float(state[DEPTH_UNITS]) / 65535.0f;
	if (state[DEPTH_FACTOR]) {
		float dx = pa.z * step_a + pb.z * step_b + pc.z * step_c;
		float dy = (pa.z * (pc.x - pb.x) + pb.z * (pa.x - pc.x) +
			    pc.z * (pb.x - pa.x)) *
			   inv_area;

		offset += state_float(state[DEPTH_FACTOR]) *
			  fmaxf(fabsf(dx), fabsf(dy));
	}

	/*
	 * Depth as a step from the first corner: a triangle of one depth, such
	 * as one drawn with an empty depth range, keeps exactly that depth.
	 */
	float z_first = pa.z + offset;
	float z_to_b = pb.z - pa.z;
	float z_to_c = pc.z - pa.z;

	uint32_t write_mask = ((flags & COLOR_WRITE) ? 0x00ffffffU : 0) |
			      ((flags & ALPHA_WRITE) ? 0xff000000U : 0);
	int rgb_start = !front && (flags & BACK_LIGHTING) ? BACK_R : R;
	bool smooth = (flags & SMOOTH) &&
		      (a->values[A] != b->values[A] ||
		       a->values[A] != c->values[A] ||
		       memcmp(a->values + rgb_start, b->values + rgb_start,
			      3 * sizeof(float)) ||
		       memcmp(a->values + rgb_start, c->values + rgb_start,
			      3 * sizeof(float)));
	const struct vertex *color_source = flags & SMOOTH ? a : flat;
	uint32_t triangle_color =
		argb(color_source->values[A], color_source->values + rgb_start);
	bool edge_a = includes_edge(&pb, &pc, area);
	bool edge_b = includes_edge(&pc, &pa, area);
	bool edge_c = includes_edge(&pa, &pb, area);
	for (int y = top; y < bottom; ++y) {
		float wa = edge(&pb, &pc, left + .5f, y + .5f) * inv_area;
		float wb = edge(&pc, &pa, left + .5f, y + .5f) * inv_area;
		float wc = edge(&pa, &pb, left + .5f, y + .5f) * inv_area;
		int position = y * target->stride + left;

		for (int x = left; x < right; ++x, ++position, wa += step_a,
			 wb += step_b, wc += step_c) {
			if (wa < 0 || (wa == 0 && !edge_a) || wb < 0 ||
			    (wb == 0 && !edge_b) || wc < 0 ||
			    (wc == 0 && !edge_c))
				continue;
			float z = z_first + wb * z_to_b + wc * z_to_c;

			if ((flags & DEPTH_TEST) &&
			    z > target->depths[position])
				continue;
			uint32_t color = triangle_color;

			if (smooth) {
				float rgb[3];
				float alpha = wa * a->values[A] +
					      wb * b->values[A] +
					      wc * c->values[A];

				for (int i = 0; i < 3; ++i)
					rgb[i] = wa * a->values[rgb_start + i] +
						 wb * b->values[rgb_start + i] +
						 wc * c->values[rgb_start + i];
				color = argb(alpha, rgb);
			}
			if (surface->texture) {
				float ua = wa, ub = wb, uc = wc;

				if (flags & PERSPECTIVE) {
					ua *= pa.inv_w;
					ub *= pb.inv_w;
					uc *= pc.inv_w;
				}
				float divisor = flags & PERSPECTIVE ?
							1 / (ua + ub + uc) :
							1;
				float u =
					(ua * a->values[U] + ub * b->values[U] +
					 uc * c->values[U]) *
					divisor;
				float v =
					(ua * a->values[V] + ub * b->values[V] +
					 uc * c->values[V]) *
					divisor;

				color = texture_color(color, surface, u, v);
			}
			if ((int)channel(color, 24) < state[THRESHOLD])
				continue;
			if (flags & FOG_ENABLED)
				color = fog_color(color,
						  (uint32_t)state[FOG_COLOR],
						  wa * a->values[FOG] +
							  wb * b->values[FOG] +
							  wc * c->values[FOG]);
			if (flags & DEPTH_WRITE)
				target->depths[position] = z;
			uint32_t under = (uint32_t)target->pixels[position];
			uint32_t value = compose(state[BLEND], color, under);

			target->pixels[position] =
				(jint)((value & write_mask) |
				       (under & ~write_mask));
		}
	}
}

/* Code of the pixel loops that must be part of each of its specialisations. */
#define INLINE static inline __attribute__((always_inline))

/*
 * Specialisations of the pixel loop. The first bits select the stages of the
 * loop; the others state facts about the triangle that the loop would
 * otherwise test for every pixel. A specialisation exists for the
 * combinations that drawing commonly produces.
 */
enum {
	MODE_TEST = 1,
	MODE_WRITE = 2,
	MODE_SMOOTH = 4,
	MODE_TEXTURE = 8,
	MODE_PERSPECTIVE = 16,
	MODE_BASE = 31,
	/* The blend is REPLACE. */
	MODE_REPLACE = 32,
	/* No alpha test and no fog, and the texture filter is not linear. */
	MODE_PLAIN = 64,
	/* How the texel and the color combine, see blend_kind(). */
	MODE_TEXEL = 128,
	MODE_MASKED = 256,
	MODE_MODULATE = 512,
	/* Both wrap modes repeat. */
	MODE_REPEAT = 1024,
};

/* Everything the pixel loop needs, so that it can keep it in locals. */
struct raster {
	jint *pixels;
	jfloat *depths;
	const jint *texture;
	const struct surface *surface;
	const struct vertex *a, *b, *c;
	struct projected pa, pb, pc;
	int stride, left, top, right, bottom;
	float inv_area, step_a, step_b, step_c;
	float z_first, z_to_b, z_to_c;
	uint32_t write_mask, triangle_color, fog_rgb, blend_color;
	int rgb_start, blend, threshold, function, width, height;
	bool edge_a, edge_b, edge_c, fog, repeat_s, repeat_t, linear;
	bool has_color, has_alpha, wide;
	int kind;
	uint32_t texel_mask;
};

INLINE float floor_float(float value)
{
	float whole;

	if (fabsf(value) >= 8388608.0f)
		return value;
	whole = (float)(int)value;
	return whole > value ? whole - 1.0f : whole;
}

/*
 * texture_coordinate with the extent conversions done once per triangle; a
 * coordinate inside the texture needs neither the finite check nor wrapping.
 */
INLINE int nearest_index(float coord, int extent, float flimit, bool repeat)
{
	if (!(coord >= 0 && coord < 1)) {
		if (!isfinite(coord))
			return 0;
		if (repeat)
			coord -= floor_float(coord);
	}
	float value = coord * (float)extent;

	if (value <= 0)
		return 0;
	if (value >= flimit)
		return extent - 1;
	return (int)value;
}

/* How the texel and the color combine, the common cases apart from the rest. */
enum {
	BLEND_TEXEL,
	BLEND_MASKED,
	BLEND_MODULATE,
	BLEND_GENERIC,
};

static int blend_kind(int function, bool has_color, bool has_alpha)
{
	if (function == TEXTURE_REPLACE)
		return has_color && has_alpha ? BLEND_TEXEL : BLEND_MASKED;
	if (has_color && function != TEXTURE_ADD && function != TEXTURE_BLEND &&
	    function != TEXTURE_DECAL)
		return BLEND_MODULATE;
	return BLEND_GENERIC;
}

/* Same as texture_color after the texel is known. */
INLINE uint32_t texture_blend(uint32_t color, uint32_t texel, int function,
			      bool has_color, bool has_alpha,
			      uint32_t blend_color)
{
	unsigned int alpha = channel(color, 24), tex_alpha = channel(texel, 24);
	uint32_t rgb = 0;

	if (function == TEXTURE_REPLACE)
		return ((has_alpha ? texel : color) & 0xff000000U) |
		       ((has_color ? texel : color) & 0x00ffffffU);
	if (!has_color) {
		if (function != TEXTURE_DECAL)
			alpha = alpha * tex_alpha / 255;
		return (color & 0x00ffffffU) | (alpha << 24);
	}
	if (color == 0xffffffffU && function != TEXTURE_ADD &&
	    function != TEXTURE_BLEND && function != TEXTURE_DECAL)
		return texel;

	for (unsigned int shift = 0; shift < 24; shift += 8) {
		unsigned int from = channel(color, shift);
		unsigned int to = channel(texel, shift);
		unsigned int value;

		switch (function) {
		case TEXTURE_DECAL:
			value = mix(from, to, tex_alpha);
			break;
		case TEXTURE_ADD:
			value = saturated(from + to);
			break;
		case TEXTURE_BLEND:
			value = mix(from, channel(blend_color, shift), to);
			break;
		default:
			value = from * to / 255;
			break;
		}
		rgb |= value << shift;
	}
	if (has_alpha && function != TEXTURE_DECAL)
		alpha = alpha * tex_alpha / 255;
	return alpha << 24 | rgb;
}

/* texture_blend for the modulate function of a texture that has color. */
INLINE uint32_t modulate(uint32_t color, uint32_t texel, bool has_alpha)
{
	unsigned int alpha = channel(color, 24);

	if (color == 0xffffffffU)
		return texel;
	if (has_alpha)
		alpha = alpha * channel(texel, 24) / 255;
	return alpha << 24 |
	       (channel(color, 16) * channel(texel, 16) / 255) << 16 |
	       (channel(color, 8) * channel(texel, 8) / 255) << 8 |
	       channel(color, 0) * channel(texel, 0) / 255;
}

/*
 * How many of the next pixels, at most limit, a running sum surely spends on
 * one side of zero, given its distance from zero in pixels (distance divided
 * by the step). The sum differs from the straight line by rounding, at most a
 * few parts in 10^5 of the distance over a row of up to 1024 pixels; the
 * count stays well within that and one pixel short.
 */
INLINE int sure_pixels(float distance, int limit)
{
	float count = distance - distance * (1.0f / 1024) - 1;

	if (!(count > 0))
		return 0;
	if (count >= (float)limit)
		return limit;
	return (int)count;
}

/*
 * The pixels of a row that pass the edge tests are consecutive: each
 * barycentric value is a running sum of one constant, so it only moves one
 * way and its test can only change once. Rows are therefore walked up to the
 * first covered pixel and left at the first uncovered pixel after it. The sums
 * and the tests are those of the generic loop. In wide rows the tests of
 * pixels that surely fail, or surely pass, are left out, as far as the
 * distance of the sums from zero allows.
 */
/* Nearly every tested value is positive: that case needs one comparison. */
#define EDGE_PASS(w, included) \
	(__builtin_expect((w) > 0, 1) || ((w) == 0 && (included)))

INLINE void raster_rows(const struct raster *r, const int mode)
{
#pragma STDC FP_CONTRACT OFF
	jint *const pixels = r->pixels;
	jfloat *const depths = r->depths;
	const jint *const texture = r->texture;
	const struct projected pa = r->pa, pb = r->pb, pc = r->pc;
	const int stride = r->stride, left = r->left, right = r->right;
	const int bottom = r->bottom;
	const float inv_area = r->inv_area;
	const float step_a = r->step_a, step_b = r->step_b, step_c = r->step_c;
	const float z_first = r->z_first, z_to_b = r->z_to_b,
		    z_to_c = r->z_to_c;
	const uint32_t write_mask = r->write_mask;
	const uint32_t keep_mask = ~write_mask;
	const uint32_t triangle_color = r->triangle_color;
	const int blend = r->blend;
	const int threshold = (mode & MODE_PLAIN) ? 0 : r->threshold;
	const bool edge_a = r->edge_a, edge_b = r->edge_b, edge_c = r->edge_c;
	const bool fog = !(mode & MODE_PLAIN) && r->fog;
	const bool whole = write_mask == 0xffffffffU;
	const float *const va = r->a->values, *const vb = r->b->values;
	const float *const vc = r->c->values;
	const int rgb_start = r->rgb_start;
	const float alpha_a = va[A], alpha_b = vb[A], alpha_c = vc[A];
	const float red_a = va[rgb_start], red_b = vb[rgb_start];
	const float red_c = vc[rgb_start];
	const float green_a = va[rgb_start + 1], green_b = vb[rgb_start + 1];
	const float green_c = vc[rgb_start + 1];
	const float blue_a = va[rgb_start + 2], blue_b = vb[rgb_start + 2];
	const float blue_c = vc[rgb_start + 2];
	const float fog_a = va[FOG], fog_b = vb[FOG], fog_c = vc[FOG];
	const float u_a = va[U], u_b = vb[U], u_c = vc[U];
	const float v_a = va[V], v_b = vb[V], v_c = vc[V];
	const float inv_w_a = pa.inv_w, inv_w_b = pb.inv_w, inv_w_c = pc.inv_w;
	const int width = r->width, height = r->height;
	const float limit_s = (float)(width - 1), limit_t = (float)(height - 1);
	const bool repeat_s = (mode & MODE_REPEAT) || r->repeat_s;
	const bool repeat_t = (mode & MODE_REPEAT) || r->repeat_t;
	const bool linear = !(mode & MODE_PLAIN) && r->linear;
	const int function = r->function;
	const bool has_color = r->has_color, has_alpha = r->has_alpha;
	const uint32_t blend_color = r->blend_color;
	const uint32_t texel_mask = r->texel_mask;
	const int kind = (mode & MODE_TEXEL)	? BLEND_TEXEL :
			 (mode & MODE_MASKED)	? BLEND_MASKED :
			 (mode & MODE_MODULATE) ? BLEND_MODULATE :
						  r->kind;
	/* The channels of the vertex color that the texture keeps. */
	const uint32_t unmasked = !(mode & MODE_TEXTURE) ? 0xffffffffU :
				  kind == BLEND_TEXEL	 ? 0 :
				  kind == BLEND_MASKED	 ? ~texel_mask :
							   0xffffffffU;
	const bool need_alpha = unmasked >> 24;
	const bool need_rgb = unmasked & 0x00ffffffU;
	/* Long rows: pixels per unit of a sum, for its distance from zero. */
	const bool wide = r->wide;
	float rise_a = 0, rise_b = 0, rise_c = 0, fall_a = 0, fall_b = 0,
	      fall_c = 0;
	bool bounded = false;

	if (wide) {
		/* Steps nearer to zero are not trusted for the distance. */
		rise_a = step_a >= 1e-9f ? 1.0f / step_a : 0;
		rise_b = step_b >= 1e-9f ? 1.0f / step_b : 0;
		rise_c = step_c >= 1e-9f ? 1.0f / step_c : 0;
		fall_a = step_a <= -1e-9f ? -1.0f / step_a : 0;
		fall_b = step_b <= -1e-9f ? -1.0f / step_b : 0;
		fall_c = step_c <= -1e-9f ? -1.0f / step_c : 0;
		bounded = (step_a >= 0 || step_a <= -1e-9f) &&
			  (step_b >= 0 || step_b <= -1e-9f) &&
			  (step_c >= 0 || step_c <= -1e-9f);
	}
	/* The edge functions of all rows share terms of the first column. */
	const float first_x = left + .5f;
	const float dx_a = pc.x - pb.x,
		    term_a = (pc.y - pb.y) * (first_x - pb.x);
	const float dx_b = pa.x - pc.x,
		    term_b = (pa.y - pc.y) * (first_x - pc.x);
	const float dx_c = pb.x - pa.x,
		    term_c = (pb.y - pa.y) * (first_x - pa.x);

	for (int y = r->top; y < bottom; ++y) {
		const float row_y = y + .5f;
		float wa = (dx_a * (row_y - pb.y) - term_a) * inv_area;
		float wb = (dx_b * (row_y - pc.y) - term_b) * inv_area;
		float wc = (dx_c * (row_y - pa.y) - term_c) * inv_area;
		int x = left;

		if (wide) {
			/* Rising sums below zero: no pixel is covered yet. */
			int skip = sure_pixels(-wa * rise_a, right - x);
			int more = sure_pixels(-wb * rise_b, right - x);

			if (more > skip)
				skip = more;
			more = sure_pixels(-wc * rise_c, right - x);
			if (more > skip)
				skip = more;
			for (int i = 0; i < skip;
			     ++i, wa += step_a, wb += step_b, wc += step_c)
				;
			x += skip;
		}
		/* Up to the first covered pixel. */
		for (;; ++x, wa += step_a, wb += step_b, wc += step_c) {
			if (x >= right)
				goto next_row;
			if (!EDGE_PASS(wa, edge_a)) {
				/* A sum that does not grow never passes. */
				if (!(step_a > 0))
					goto next_row;
				continue;
			}
			if (!EDGE_PASS(wb, edge_b)) {
				if (!(step_b > 0))
					goto next_row;
				continue;
			}
			if (!EDGE_PASS(wc, edge_c)) {
				if (!(step_c > 0))
					goto next_row;
				continue;
			}
			break;
		}
		jint *pixel = pixels + (y * stride + x);
		jfloat *depth = depths + (y * stride + x);

		/* Falling sums that stay above zero need no test. */
		int tested = x;

		if (wide) {
			int sure = right - x;

			if (!bounded) {
				sure = 0;
			} else {
				if (fall_a > 0)
					sure = sure_pixels(wa * fall_a, sure);
				if (fall_b > 0)
					sure = sure_pixels(wb * fall_b, sure);
				if (fall_c > 0)
					sure = sure_pixels(wc * fall_c, sure);
			}
			tested = x + sure;
		}
		for (; x < right; ++x, ++pixel, ++depth, wa += step_a,
				  wb += step_b, wc += step_c) {
			if (x >= tested &&
			    (!EDGE_PASS(wa, edge_a) || !EDGE_PASS(wb, edge_b) ||
			     !EDGE_PASS(wc, edge_c)))
				break;
			float z = z_first + wb * z_to_b + wc * z_to_c;

			if ((mode & MODE_TEST) && z > *depth)
				continue;
			uint32_t color = triangle_color;

			if (mode & MODE_SMOOTH) {
				/* Channels that the texture replaces stay 0. */
				color = 0;
				if (need_alpha)
					color = color_component_bounded(
							wa * alpha_a +
							wb * alpha_b +
							wc * alpha_c)
						<< 24;
				if (need_rgb) {
					float red = wa * red_a + wb * red_b +
						    wc * red_c;
					float green = wa * green_a +
						      wb * green_b +
						      wc * green_c;
					float blue = wa * blue_a + wb * blue_b +
						     wc * blue_c;

					color |= color_component_bounded(red)
							 << 16 |
						 color_component_bounded(green)
							 << 8 |
						 color_component_bounded(blue);
				}
			}
			if (mode & MODE_TEXTURE) {
				float ua = wa, ub = wb, uc = wc;

				if (mode & MODE_PERSPECTIVE) {
					ua *= inv_w_a;
					ub *= inv_w_b;
					uc *= inv_w_c;
				}
				float divisor = (mode & MODE_PERSPECTIVE) ?
							1 / (ua + ub + uc) :
							1;
				float u = (ua * u_a + ub * u_b + uc * u_c) *
					  divisor;
				float v = (ua * v_a + ub * v_b + uc * v_c) *
					  divisor;
				uint32_t texel;

				if (linear) {
					texel = sample_linear(r->surface, u, v);
				} else {
					int s = nearest_index(u, width, limit_s,
							      repeat_s);
					int t = nearest_index(
						v, height, limit_t, repeat_t);

					texel = (uint32_t)texture[t * width + s];
				}
				switch (kind) {
				case BLEND_TEXEL:
					color = texel;
					break;
				case BLEND_MASKED:
					color = (texel & texel_mask) |
						(color & ~texel_mask);
					break;
				case BLEND_MODULATE:
					color = modulate(color, texel,
							 has_alpha);
					break;
				default:
					color = texture_blend(color, texel,
							      function,
							      has_color,
							      has_alpha,
							      blend_color);
					break;
				}
			}
			if (threshold > 0 &&
			    (int)channel(color, 24) < threshold)
				continue;
			if (fog)
				color = fog_color(color, r->fog_rgb,
						  wa * fog_a + wb * fog_b +
							  wc * fog_c);
			if (mode & MODE_WRITE)
				*depth = z;
			if ((mode & MODE_REPLACE) || blend == REPLACE) {
				if (whole)
					*pixel = (jint)color;
				else
					*pixel = (jint)((color & write_mask) |
							((uint32_t)*pixel &
							 keep_mask));
			} else {
				uint32_t under = (uint32_t)*pixel;
				uint32_t value;

				if (blend != ALPHA)
					value = compose(blend, color, under);
				else if (color >> 24 == 255)
					value = color;
				else if (color >> 24 == 0)
					continue;
				else
					value = blend_alpha(color, under,
							    color >> 24);
				*pixel = (jint)((value & write_mask) |
						(under & keep_mask));
			}
		}
next_row:;
	}
}

/* The loop for any state: the facts that mode does not give are tested. */
static __attribute__((noinline)) void raster_runtime(const struct raster *r,
						     int mode)
{
	raster_rows(r, mode);
}

/*
 * Loops with the state that games commonly draw with built in: the exact mode
 * and the function that runs it.
 * A color that the texture replaces entirely is not interpolated, so
 * MODE_SMOOTH is not part of the texel loops.
 */
#define TEXTURED (MODE_TEST | MODE_TEXTURE | MODE_PERSPECTIVE | MODE_REPLACE)
#define VARIANTS(X)                                                         \
	X(raster_solid, MODE_TEST | MODE_WRITE | MODE_REPLACE | MODE_PLAIN) \
	X(raster_gouraud,                                                   \
	  MODE_TEST | MODE_WRITE | MODE_SMOOTH | MODE_REPLACE | MODE_PLAIN) \
	X(raster_masked, MODE_TEST | MODE_WRITE | MODE_TEXTURE |            \
				 MODE_REPLACE | MODE_PLAIN | MODE_MASKED)   \
	X(raster_masked_smooth, MODE_TEST | MODE_WRITE | MODE_SMOOTH |      \
					MODE_TEXTURE | MODE_REPLACE |       \
					MODE_PLAIN | MODE_MASKED)           \
	X(raster_modulate_smooth, TEXTURED | MODE_WRITE | MODE_SMOOTH |     \
					  MODE_PLAIN | MODE_MODULATE |      \
					  MODE_REPEAT)                      \
	X(raster_texel_write,                                               \
	  TEXTURED | MODE_WRITE | MODE_PLAIN | MODE_TEXEL | MODE_REPEAT)    \
	X(raster_texel,                                                     \
	  (TEXTURED & ~MODE_REPLACE) | MODE_PLAIN | MODE_TEXEL | MODE_REPEAT)

#define DEFINE_VARIANT(name, bits)                                         \
	static __attribute__((noinline)) void name(const struct raster *r) \
	{                                                                  \
		raster_rows(r, bits);                                      \
	}

VARIANTS(DEFINE_VARIANT)

/* fminf and fmaxf for values that are not not-a-number, without a call. */
static inline float smaller(float a, float b)
{
	return b < a ? b : a;
}

static inline float larger(float a, float b)
{
	return b > a ? b : a;
}

/*
 * Rasterizes one triangle. Corners with a positive w are projected, the
 * state is gathered once and the pixel loop of the matching specialisation
 * runs. Values that would make the sums of the pixel loop unordered (not
 * finite) take the generic loop.
 */
static void rasterize(struct target *target, const struct surface *surface,
		      const struct vertex *a, const struct vertex *b,
		      const struct vertex *c, const struct vertex *flat)
{
#pragma STDC FP_CONTRACT OFF
	const jint *state = surface->state;
	const struct rectangle *clip = &target->clip;
	int flags = state[FLAGS];

	if (!(a->values[W] > 0 && b->values[W] > 0 && c->values[W] > 0))
		return;
	struct projected pa = project(target, a), pb = project(target, b);
	struct projected pc = project(target, c);
	float area = edge(&pa, &pb, pc.x, pc.y);
	bool front = state[WINDING] == WINDING_CCW ? area < 0 : area > 0;

	if (!isfinite(area) || area == 0 ||
	    (state[CULLING] == CULL_BACK && !front) ||
	    (state[CULLING] == CULL_FRONT && front))
		return;

	/* The area is finite, so no corner is not-a-number. */
	int left = lower_bound(smaller(pa.x, smaller(pb.x, pc.x)), clip->left,
			       clip->right);
	int top = lower_bound(smaller(pa.y, smaller(pb.y, pc.y)), clip->top,
			      clip->bottom);
	int right = upper_bound(larger(pa.x, larger(pb.x, pc.x)), clip->left,
				clip->right);
	int bottom = upper_bound(larger(pa.y, larger(pb.y, pc.y)), clip->top,
				 clip->bottom);
	if (left >= right || top >= bottom)
		return;

	float inv_area = 1.0f / area;
	float step_a = (pb.y - pc.y) * inv_area;
	float step_b = (pc.y - pa.y) * inv_area;
	float step_c = (pa.y - pb.y) * inv_area;
	/* Every sum is then finite or infinite, never undefined. */
	if (!(fabsf(pa.x) + fabsf(pa.y) + fabsf(pb.x) + fabsf(pb.y) +
		      fabsf(pc.x) + fabsf(pc.y) <
	      1e9f) ||
	    !(fabsf(inv_area) + fabsf(step_a) + fabsf(step_b) + fabsf(step_c) <
	      INFINITY)) {
		rasterize_reference(target, surface, a, b, c, flat);
		return;
	}
	float offset = state_float(state[DEPTH_UNITS]) / 65535.0f;
	if (state[DEPTH_FACTOR]) {
		float dx = pa.z * step_a + pb.z * step_b + pc.z * step_c;
		float dy = (pa.z * (pc.x - pb.x) + pb.z * (pa.x - pc.x) +
			    pc.z * (pb.x - pa.x)) *
			   inv_area;

		offset += state_float(state[DEPTH_FACTOR]) *
			  fmaxf(fabsf(dx), fabsf(dy));
	}

	int rgb_start = !front && (flags & BACK_LIGHTING) ? BACK_R : R;
	uint32_t first[3], second[3], third[3];

	memcpy(first, a->values + rgb_start, sizeof(first));
	memcpy(second, b->values + rgb_start, sizeof(second));
	memcpy(third, c->values + rgb_start, sizeof(third));
	bool smooth = (flags & SMOOTH) &&
		      (a->values[A] != b->values[A] ||
		       a->values[A] != c->values[A] || first[0] != second[0] ||
		       first[1] != second[1] || first[2] != second[2] ||
		       first[0] != third[0] || first[1] != third[1] ||
		       first[2] != third[2]);
	if (smooth) {
		/*
		 * The pixel loop converts the interpolated colors without range
		 * tests, see color_component_bounded(). A covered pixel is
		 * within the box of the corners, so an edge function is at
		 * most 2 * box (width + 2 times height + 2), and a row start
		 * plus the steps along the row give a weight of at most
		 * 3 * box * inv_area; the factor 2 covers rounding. A color
		 * is at most 3 * weight times the sum of the absolute color
		 * values. If that could reach 2^30, or is not a number, the
		 * generic loop runs.
		 */
		float width_x = larger(pa.x, larger(pb.x, pc.x)) -
				smaller(pa.x, smaller(pb.x, pc.x));
		float width_y = larger(pa.y, larger(pb.y, pc.y)) -
				smaller(pa.y, smaller(pb.y, pc.y));
		float weight =
			6 * (width_x + 2) * (width_y + 2) * fabsf(inv_area);
		float magnitude = 0;

		for (int i = 0; i < 4; ++i) {
			int index = i ? rgb_start + i - 1 : A;

			magnitude += fabsf(a->values[index]) +
				     fabsf(b->values[index]) +
				     fabsf(c->values[index]);
		}
		if (!(3 * weight * magnitude < 1e9f)) {
			rasterize_reference(target, surface, a, b, c, flat);
			return;
		}
	}
	const struct vertex *color_source = flags & SMOOTH ? a : flat;
	int format = state[TEXTURE_FORMAT];
	struct raster raster = {
		.pixels = target->pixels,
		.depths = target->depths,
		.texture = surface->texture,
		.surface = surface,
		.a = a,
		.b = b,
		.c = c,
		.pa = pa,
		.pb = pb,
		.pc = pc,
		.stride = target->stride,
		.left = left,
		.top = top,
		.right = right,
		.bottom = bottom,
		.inv_area = inv_area,
		.step_a = step_a,
		.step_b = step_b,
		.step_c = step_c,
		/*
		 * Depth as a step from the first corner: a triangle of one
		 * depth, such as one drawn with an empty depth range, keeps
		 * exactly that depth.
		 */
		.z_first = pa.z + offset,
		.z_to_b = pb.z - pa.z,
		.z_to_c = pc.z - pa.z,
		.write_mask = ((flags & COLOR_WRITE) ? 0x00ffffffU : 0) |
			      ((flags & ALPHA_WRITE) ? 0xff000000U : 0),
		.triangle_color =
			smooth ? 0 :
				 argb(color_source->values[A],
				      color_source->values + rgb_start),
		.fog_rgb = (uint32_t)state[FOG_COLOR],
		.blend_color = (uint32_t)state[BLEND_COLOR],
		.rgb_start = rgb_start,
		.blend = state[BLEND],
		.threshold = state[THRESHOLD],
		.function = state[TEXTURE_FUNCTION],
		.width = state[TEXTURE_WIDTH],
		.height = state[TEXTURE_HEIGHT],
		.edge_a = includes_edge(&pb, &pc, area),
		.edge_b = includes_edge(&pc, &pa, area),
		.edge_c = includes_edge(&pa, &pb, area),
		.fog = flags & FOG_ENABLED,
		.repeat_s = state[WRAP_S] == WRAP_REPEAT,
		.repeat_t = state[WRAP_T] == WRAP_REPEAT,
		.linear = surface->texture &&
			  state[IMAGE_FILTER] == FILTER_LINEAR,
		.has_color = format != IMAGE_ALPHA,
		.has_alpha = format != IMAGE_LUMINANCE && format != IMAGE_RGB,
	};
	raster.kind =
		blend_kind(raster.function, raster.has_color, raster.has_alpha);
	raster.texel_mask = (raster.has_alpha ? 0xff000000U : 0) |
			    (raster.has_color ? 0x00ffffffU : 0);
	int mode = ((flags & DEPTH_TEST) ? MODE_TEST : 0) |
		   ((flags & DEPTH_WRITE) ? MODE_WRITE : 0) |
		   (smooth ? MODE_SMOOTH : 0);

	if (raster.blend == REPLACE)
		mode |= MODE_REPLACE;
	if (!raster.fog && raster.threshold <= 0 && !raster.linear)
		mode |= MODE_PLAIN;
	if (surface->texture) {
		mode |= MODE_TEXTURE |
			((flags & PERSPECTIVE) ? MODE_PERSPECTIVE : 0) |
			(raster.repeat_s && raster.repeat_t ? MODE_REPEAT : 0);
		if (raster.kind == BLEND_TEXEL)
			mode = (mode | MODE_TEXEL) & ~MODE_SMOOTH;
		else if (raster.kind == BLEND_MASKED)
			mode |= MODE_MASKED;
		else if (raster.kind == BLEND_MODULATE)
			mode |= MODE_MODULATE;
	}
	raster.wide = right - left >= 16 && right - left <= 1024;
	switch (mode) {
#define RUN_VARIANT(name, bits) \
	case bits:              \
		name(&raster);  \
		return;
		VARIANTS(RUN_VARIANT)
	}
	raster_runtime(&raster, mode & MODE_BASE);
}

static float distance(const struct vertex *vertex, unsigned int plane)
{
	const float *v = vertex->values;

	return v[W] + ((plane & 1) ? -v[plane / 2] : v[plane / 2]);
}

enum { CLIPPED_VERTEX_COUNT = 9 };

static size_t clip_plane(const struct vertex *input, size_t count,
			 struct vertex *output, unsigned int plane)
{
	const struct vertex *previous = &input[count - 1];
	float from = distance(previous, plane);
	size_t result = 0;

	for (size_t i = 0; i < count; ++i) {
		const struct vertex *current = &input[i];
		float to = distance(current, plane);

		if ((from < 0 && to > 0) || (from > 0 && to < 0)) {
			if (result == CLIPPED_VERTEX_COUNT)
				return 0;
			float t = from / (from - to);
			float *values = output[result++].values;

			for (size_t j = 0; j < VERTEX_COMPONENTS; ++j) {
				float a = previous->values[j];

				values[j] = a + t * (current->values[j] - a);
			}
		}
		if (to >= 0) {
			if (result == CLIPPED_VERTEX_COUNT)
				return 0;
			output[result++] = *current;
		}
		previous = current;
		from = to;
	}
	return result;
}

static void draw_triangle(struct target *target, const struct surface *surface,
			  const jfloat *vertices, int a, int b, int c)
{
	/* Each vertex is VERTEX_COMPONENTS consecutive floats of the array. */
	const struct vertex *corners[3] = {
		(const struct vertex *)(vertices + a * VERTEX_COMPONENTS),
		(const struct vertex *)(vertices + b * VERTEX_COMPONENTS),
		(const struct vertex *)(vertices + c * VERTEX_COMPONENTS),
	};
	unsigned int outside = 0;

	for (size_t vertex = 0; vertex < 3; ++vertex) {
		for (size_t component = 0; component <= W; ++component)
			if (!isfinite(corners[vertex]->values[component]))
				return;
	}
	for (unsigned int plane = 0; plane < 6; ++plane) {
		int n = (distance(corners[0], plane) < 0) +
			(distance(corners[1], plane) < 0) +
			(distance(corners[2], plane) < 0);

		if (n == 3)
			return;
		if (n)
			outside |= 1U << plane;
	}
	if (!outside) {
		/* Only clipping needs copies of the corners. */
		rasterize(target, surface, corners[0], corners[1], corners[2],
			  corners[2]);
		return;
	}

	struct vertex work[2][CLIPPED_VERTEX_COUNT];
	size_t count = 3;
	struct vertex *input = work[0];

	for (size_t vertex = 0; vertex < 3; ++vertex)
		input[vertex] = *corners[vertex];
	const struct vertex flat = input[2];
	struct vertex *output = work[1];
	for (unsigned int plane = 0; plane < 6; ++plane) {
		if (!(outside & (1U << plane)))
			continue;
		count = clip_plane(input, count, output, plane);
		if (count < 3)
			return;
		struct vertex *previous = input;

		input = output;
		output = previous;
	}
	for (size_t i = 2; i < count; ++i)
		rasterize(target, surface, &input[0], &input[i - 1], &input[i],
			  &flat);
}

static bool draw_strips(struct target *target, const struct surface *surface,
			const jfloat *vertices, int vertex_count,
			const jint *strips, int strip_count,
			const jint *indices, int index_count)
{
	int offset = 0;

	for (int strip = 0; strip < strip_count; ++strip) {
		int length = strips[strip];

		if (length < 3 || length > index_count - offset)
			return false;
		int end = offset + length;
		for (int i = offset + 2; i < end; ++i) {
			int a = indices[i - 2], b = indices[i - 1],
			    c = indices[i];

			if (a < 0 || b < 0 || c < 0 || a >= vertex_count ||
			    b >= vertex_count || c >= vertex_count)
				return false;
			if (a == b || b == c || a == c)
				continue;
			if ((i - offset) & 1)
				draw_triangle(target, surface, vertices, b, a,
					      c);
			else
				draw_triangle(target, surface, vertices, a, b,
					      c);
		}
		offset = end;
	}
	return offset == index_count;
}

static int background_index(int position, int origin, int view_size, int start,
			    int crop_size, int extent, int mode)
{
	int64_t value =
		((int64_t)position - origin) * crop_size / view_size + start;

	if (value >= 0 && value < extent)
		return (int)value;
	if (mode != BACKGROUND_REPEAT)
		return -1;
	int index = (int)(value % extent);

	return index < 0 ? index + extent : index;
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_m3g_Rasterizer_bindDepth0)
{
	jint count = KNI_GetParameterAsInt(1);

	if (count > depth_capacity) {
		/* Zeroed like the Java array it replaces. */
		jfloat *buffer = calloc((size_t)count, sizeof(*buffer));

		if (buffer == NULL) {
			KNI_ThrowNew("java/lang/OutOfMemoryError",
				     "M3G depth buffer");
		} else {
			free(depth_buffer);
			depth_buffer = buffer;
			depth_capacity = count;
			depth_cleared = (struct rectangle){ 0 };
		}
	}
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(javax_microedition_m3g_Rasterizer_clear0)
{
	struct target target;
	jint color = KNI_GetParameterAsInt(3);
	jboolean color_clear = KNI_GetParameterAsBoolean(4);
	jboolean depth_clear = KNI_GetParameterAsBoolean(5);
	uint32_t alpha = KNI_GetParameterAsBoolean(8) ? 0xff000000U : 0;

	KNI_StartHandles(4);
	KNI_DeclareHandle(pixels);
	KNI_DeclareHandle(view);
	KNI_DeclareHandle(image);
	KNI_DeclareHandle(crop);
	KNI_GetParameterAsObject(1, pixels);
	KNI_GetParameterAsObject(2, view);
	KNI_GetParameterAsObject(6, image);
	KNI_GetParameterAsObject(7, crop);
	if (!target_init(&target, pixels, view)) {
		KNI_ThrowNew("java/lang/IllegalArgumentException",
			     "M3G target");
	} else if (!empty(&target.clip)) {
		const struct rectangle *clip = &target.clip;
		const jint *layout = SNI_GetRawArrayPointer(view);
		const jint *region = NULL, *source = NULL;

		if (color_clear && !KNI_IsNullHandle(image) &&
		    !KNI_IsNullHandle(crop) && KNI_GetArrayLength(crop) == 8) {
			region = SNI_GetRawArrayPointer(crop);
			if (region[0] > 0 && region[1] > 0 && region[4] > 0 &&
			    region[5] > 0 &&
			    (int64_t)region[0] * region[1] <=
				    KNI_GetArrayLength(image))
				source = SNI_GetRawArrayPointer(image);
		}

		if (color_clear && !source)
			fill_rectangle(target.pixels, target.stride, clip,
				       &color);
		for (int y = clip->top;
		     color_clear && source && y < clip->bottom; ++y) {
			jint *pixel_row = target.pixels + y * target.stride;
			int sy = background_index(y, layout[VIEW_Y],
						  layout[VIEWPORT_HEIGHT],
						  region[3], region[5],
						  region[1], region[7]);

			if (sy < 0) {
				for (int x = clip->left; x < clip->right; ++x)
					pixel_row[x] = color;
			} else {
				for (int x = clip->left; x < clip->right; ++x) {
					int sx = background_index(
						x, layout[VIEW_X],
						layout[VIEWPORT_WIDTH],
						region[2], region[4], region[0],
						region[6]);

					pixel_row[x] =
						sx < 0 ?
							color :
							(jint)((uint32_t)source
								       [sy * region[0] +
									sx] |
							       alpha);
				}
			}
		}
		if (depth_clear && (depth_cleared_stride != target.stride ||
				    !contains(&depth_cleared, clip))) {
			const jfloat far = 1;

			fill_rectangle(target.depths, target.stride, clip,
				       &far);
			/* Keep the larger of the known cleared parts. */
			if (depth_cleared_stride != target.stride ||
			    empty(&depth_cleared) ||
			    contains(clip, &depth_cleared)) {
				depth_cleared = *clip;
				depth_cleared_stride = target.stride;
			}
		}
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_FLOAT KNIDECL(javax_microedition_m3g_Rasterizer_pow0)
{
	float value = KNI_GetParameterAsFloat(1);
	float exponent = KNI_GetParameterAsFloat(2);

	KNI_ReturnFloat(powf(value, exponent));
}

KNIEXPORT KNI_RETURNTYPE_FLOAT KNIDECL(javax_microedition_m3g_Fog_exp0)
{
	KNI_ReturnFloat(expf(KNI_GetParameterAsFloat(1)));
}

KNIEXPORT KNI_RETURNTYPE_FLOAT
KNIDECL(javax_microedition_m3g_Transformable_angleDegrees0)
{
	float half_sine = KNI_GetParameterAsFloat(1);
	float half_cosine = KNI_GetParameterAsFloat(2);

	KNI_ReturnFloat(atan2f(half_sine, half_cosine) *
			(360.0f / 3.141592653589793f));
}

/*
 * Transform matrices are 16 floats in rows. The float operations and their
 * order are those of the Java code the natives replace.
 */
static void multiply_matrices(jfloat *left, const jfloat *right)
{
#pragma STDC FP_CONTRACT OFF
	float result[16];

	for (int i = 0; i < 16; i += 4) {
		for (int j = 0; j < 4; ++j)
			result[i + j] = left[i] * right[j] +
					left[i + 1] * right[4 + j] +
					left[i + 2] * right[8 + j] +
					left[i + 3] * right[12 + j];
	}
	memcpy(left, result, sizeof(result));
}

/* Gauss-Jordan elimination with partial pivoting on [matrix | identity]. */
static bool invert_matrix(jfloat *matrix)
{
#pragma STDC FP_CONTRACT OFF
	float storage[4][8] = { { 0 } };
	float *rows[4];

	for (int i = 0; i < 4; ++i) {
		rows[i] = storage[i];
		memcpy(rows[i], matrix + 4 * i, 4 * sizeof(float));
		rows[i][i + 4] = 1;
	}
	for (int column = 0; column < 4; ++column) {
		int pivot = column;

		for (int candidate = column + 1; candidate < 4; ++candidate) {
			if (fabsf(rows[candidate][column]) >
			    fabsf(rows[pivot][column]))
				pivot = candidate;
		}
		float *basis = rows[pivot];

		if (basis[column] == 0)
			return false;
		rows[pivot] = rows[column];
		rows[column] = basis;
		float divisor = basis[column];

		for (int k = column; k < 8; ++k)
			basis[k] /= divisor;
		for (int row = 0; row < 4; ++row) {
			if (row == column)
				continue;
			float coefficient = rows[row][column];

			for (int k = column; k < 8; ++k)
				rows[row][k] -= basis[k] * coefficient;
		}
	}
	for (int i = 0; i < 4; ++i)
		memcpy(matrix + 4 * i, rows[i] + 4, 4 * sizeof(float));
	return true;
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(javax_microedition_m3g_Transform_multiply0)
{
	KNI_StartHandles(2);
	KNI_DeclareHandle(left);
	KNI_DeclareHandle(right);
	KNI_GetParameterAsObject(1, left);
	KNI_GetParameterAsObject(2, right);
	multiply_matrices(SNI_GetRawArrayPointer(left),
			  SNI_GetRawArrayPointer(right));
	KNI_EndHandles();
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(javax_microedition_m3g_Transform_invert0)
{
	bool inverted;

	KNI_StartHandles(1);
	KNI_DeclareHandle(matrix);
	KNI_GetParameterAsObject(1, matrix);
	inverted = invert_matrix(SNI_GetRawArrayPointer(matrix));
	KNI_EndHandles();
	KNI_ReturnBoolean(inverted);
}

struct coordinates {
	const jshort *values;
	const jfloat *scale_bias;
	int components;
};

static float transform_row(const jfloat *row, float x, float y, float z)
{
#pragma STDC FP_CONTRACT OFF
	return row[0] * x + row[1] * y + row[2] * z + row[3];
}

static void prepare_vertices(jfloat *vertices, int count,
			     const struct coordinates *positions,
			     const jfloat *projection,
			     const struct coordinates *tex_coords,
			     const jfloat *texture, bool constant_color,
			     uint32_t color, float alpha)
{
	/* Match the separate float operations in the Java transforms. */
#pragma STDC FP_CONTRACT OFF
	/*
	 * Without strict aliasing the stores to vertices could change these
	 * inputs; local copies keep them from being reloaded for every vertex.
	 */
	const struct coordinates position_input = *positions;
	const struct coordinates tex_input = *tex_coords;
	float position[4], matrix[16], tex_matrix[16], tex_bias[4];

	memcpy(position, position_input.scale_bias, sizeof(position));
	memcpy(matrix, projection, sizeof(matrix));
	if (texture)
		memcpy(tex_matrix, texture, sizeof(tex_matrix));
	if (texture && tex_input.values)
		memcpy(tex_bias, tex_input.scale_bias, sizeof(tex_bias));
	float a = (color >> 24) * alpha;
	float r = (color >> 16) & 255U;
	float g = (color >> 8) & 255U;
	float b = color & 255U;

	for (int i = 0; i < count; ++i, vertices += VERTEX_COMPONENTS) {
		const jshort *point =
			position_input.values + i * position_input.components;
		float x = point[0] * position[0] + position[1];
		float y = point[1] * position[0] + position[2];
		float z = (position_input.components > 2 ? point[2] : 0) *
				  position[0] +
			  position[3];

		vertices[X] = transform_row(matrix, x, y, z);
		vertices[Y] = transform_row(matrix + 4, x, y, z);
		vertices[Z] = transform_row(matrix + 8, x, y, z);
		vertices[W] = transform_row(matrix + 12, x, y, z);
		if (texture) {
			/* p is the third coordinate, which M3G calls r. */
			float s = 0, t = 0, p = 0;

			if (tex_input.values) {
				const jshort *coords = tex_input.values +
						       i * tex_input.components;

				s = coords[0] * tex_bias[0] + tex_bias[1];
				t = coords[1] * tex_bias[0] + tex_bias[2];
				if (tex_input.components > 2)
					p = coords[2] * tex_bias[0] +
					    tex_bias[3];
			}
			float u = transform_row(tex_matrix, s, t, p);
			float v = transform_row(tex_matrix + 4, s, t, p);
			float q = transform_row(tex_matrix + 12, s, t, p);

			vertices[U] = u / q;
			vertices[V] = v / q;
		} else {
			vertices[U] = 0;
			vertices[V] = 0;
		}
		if (constant_color) {
			vertices[A] = a;
			vertices[R] = r;
			vertices[G] = g;
			vertices[B] = b;
		}
	}
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_m3g_Rasterizer_prepareVertices0)
{
	struct coordinates positions = { 0 }, tex_coords = { 0 };
	int count = KNI_GetParameterAsInt(2);
	int first = KNI_GetParameterAsInt(14);
	const char *exception = "java/lang/IllegalArgumentException";

	positions.components = KNI_GetParameterAsInt(4);
	tex_coords.components = KNI_GetParameterAsInt(8);
	KNI_StartHandles(7);
	KNI_DeclareHandle(vertices);
	KNI_DeclareHandle(position_values);
	KNI_DeclareHandle(position_scale_bias);
	KNI_DeclareHandle(projection);
	KNI_DeclareHandle(tex_values);
	KNI_DeclareHandle(tex_scale_bias);
	KNI_DeclareHandle(texture_matrix);
	KNI_GetParameterAsObject(1, vertices);
	KNI_GetParameterAsObject(3, position_values);
	KNI_GetParameterAsObject(5, position_scale_bias);
	KNI_GetParameterAsObject(6, projection);
	KNI_GetParameterAsObject(7, tex_values);
	KNI_GetParameterAsObject(9, tex_scale_bias);
	KNI_GetParameterAsObject(10, texture_matrix);
	if (count < 1 || first < 0 || first >= count ||
	    KNI_IsNullHandle(vertices) ||
	    count > KNI_GetArrayLength(vertices) / VERTEX_COMPONENTS ||
	    positions.components < 2 || positions.components > 4 ||
	    KNI_IsNullHandle(position_values) ||
	    count > KNI_GetArrayLength(position_values) /
			    positions.components ||
	    KNI_IsNullHandle(position_scale_bias) ||
	    KNI_GetArrayLength(position_scale_bias) < 4 ||
	    KNI_IsNullHandle(projection) || KNI_GetArrayLength(projection) < 16)
		goto done;

	const jfloat *texture = NULL;

	if (!KNI_IsNullHandle(texture_matrix)) {
		if (KNI_GetArrayLength(texture_matrix) < 16)
			goto done;
		texture = SNI_GetRawArrayPointer(texture_matrix);
		if (!KNI_IsNullHandle(tex_values)) {
			if (tex_coords.components < 2 ||
			    tex_coords.components > 4 ||
			    KNI_IsNullHandle(tex_scale_bias) ||
			    KNI_GetArrayLength(tex_scale_bias) < 4)
				goto done;
			if (count > KNI_GetArrayLength(tex_values) /
					    tex_coords.components) {
				exception =
					"java/lang/ArrayIndexOutOfBoundsException";
				goto done;
			}
			tex_coords.values =
				(const jshort *)SNI_GetRawArrayPointer(
					tex_values) +
				first * tex_coords.components;
			tex_coords.scale_bias =
				SNI_GetRawArrayPointer(tex_scale_bias);
		}
	}
	positions.values =
		(const jshort *)SNI_GetRawArrayPointer(position_values) +
		first * positions.components;
	positions.scale_bias = SNI_GetRawArrayPointer(position_scale_bias);
	prepare_vertices((jfloat *)SNI_GetRawArrayPointer(vertices) +
				 first * VERTEX_COMPONENTS,
			 count - first, &positions,
			 SNI_GetRawArrayPointer(projection), &tex_coords,
			 texture, KNI_GetParameterAsBoolean(11),
			 (uint32_t)KNI_GetParameterAsInt(12),
			 KNI_GetParameterAsFloat(13));
	exception = NULL;

done:
	if (exception)
		KNI_ThrowNew(exception, "M3G vertex preparation");
	KNI_EndHandles();
	KNI_ReturnVoid();
}

struct directional_lighting {
	uint32_t emissive, ambient, diffuse;
	uint32_t ambient_color, directional_color;
	float ambient_intensity, directional_intensity;
	bool directional_first;
};

static bool shade_directional_vertices(jfloat *vertices, int count,
				       const jshort *normals,
				       const jfloat *inverse,
				       const jfloat *direction,
				       const struct directional_lighting *light,
				       float alpha)
{
	/* Match the Java float operations and light accumulation order. */
#pragma STDC FP_CONTRACT OFF
	const unsigned int shifts[] = { 16, 8, 0 };
	float emissive[3], ambient[3], base[3], diffuse[3], directional[3];
	float ambient_gain = light->ambient_intensity / 255;
	float directional_gain = light->directional_intensity / 255;

	for (int axis = 0; axis < 3; ++axis) {
		if (!isfinite(direction[axis]) || !isfinite(inverse[axis]) ||
		    !isfinite(inverse[4 + axis]) ||
		    !isfinite(inverse[8 + axis]))
			return false;
		unsigned int shift = shifts[axis];

		emissive[axis] = channel(light->emissive, shift);
		ambient[axis] =
			channel(light->ambient_color, shift) *
			(ambient_gain * channel(light->ambient, shift) + .0f);
		base[axis] = emissive[axis] + ambient[axis];
		diffuse[axis] = channel(light->diffuse, shift);
		directional[axis] = channel(light->directional_color, shift);
	}

	for (int i = 0; i < count;
	     ++i, normals += 3, vertices += VERTEX_COMPONENTS) {
		float normal[3];

		for (int axis = 0; axis < 3; ++axis)
			normal[axis] = normals[0] * inverse[axis] +
				       normals[1] * inverse[4 + axis] +
				       normals[2] * inverse[8 + axis];
		float length = (float)sqrt(normal[0] * normal[0] +
					   normal[1] * normal[1] +
					   normal[2] * normal[2]);

		if (length != 0) {
			for (int axis = 0; axis < 3; ++axis)
				normal[axis] /= length;
		}
		float facing = normal[0] * direction[0] +
			       normal[1] * direction[1] +
			       normal[2] * direction[2];
		float gain = directional_gain * facing;

		vertices[A] = alpha;
		for (int axis = 0; axis < 3; ++axis) {
			float color = base[axis];

			if (!(facing <= 0)) {
				float reflected = directional[axis] *
						  (gain * diffuse[axis] + .0f);

				if (light->directional_first)
					color = (emissive[axis] + reflected) +
						ambient[axis];
				else
					color += reflected;
			}
			vertices[R + axis] = color_component(color);
		}
	}
	return true;
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(javax_microedition_m3g_Rasterizer_shadeDirectional0)
{
	int count = KNI_GetParameterAsInt(2);
	const struct directional_lighting light = {
		.emissive = (uint32_t)KNI_GetParameterAsInt(5),
		.ambient = (uint32_t)KNI_GetParameterAsInt(6),
		.diffuse = (uint32_t)KNI_GetParameterAsInt(7),
		.ambient_color = (uint32_t)KNI_GetParameterAsInt(8),
		.ambient_intensity = KNI_GetParameterAsFloat(9),
		.directional_color = (uint32_t)KNI_GetParameterAsInt(10),
		.directional_intensity = KNI_GetParameterAsFloat(11),
		.directional_first = KNI_GetParameterAsBoolean(13),
	};
	float alpha = channel(light.diffuse, 24) * KNI_GetParameterAsFloat(14);
	int first = KNI_GetParameterAsInt(15);
	jboolean shaded = KNI_FALSE;

	KNI_StartHandles(4);
	KNI_DeclareHandle(vertices);
	KNI_DeclareHandle(normals);
	KNI_DeclareHandle(inverse);
	KNI_DeclareHandle(direction);
	KNI_GetParameterAsObject(1, vertices);
	KNI_GetParameterAsObject(3, normals);
	KNI_GetParameterAsObject(4, inverse);
	KNI_GetParameterAsObject(12, direction);

	if (count > 0 && first >= 0 && first < count &&
	    !KNI_IsNullHandle(vertices) &&
	    count <= KNI_GetArrayLength(vertices) / VERTEX_COMPONENTS &&
	    !KNI_IsNullHandle(normals) &&
	    count <= KNI_GetArrayLength(normals) / 3 &&
	    !KNI_IsNullHandle(inverse) && KNI_GetArrayLength(inverse) >= 16 &&
	    !KNI_IsNullHandle(direction) &&
	    KNI_GetArrayLength(direction) >= 3 &&
	    isfinite(light.ambient_intensity) &&
	    isfinite(light.directional_intensity)) {
		SNI_BEGIN_RAW_POINTERS
		shaded = shade_directional_vertices(
			(jfloat *)SNI_GetRawArrayPointer(vertices) +
				first * VERTEX_COMPONENTS,
			count - first,
			(const jshort *)SNI_GetRawArrayPointer(normals) +
				first * 3,
			SNI_GetRawArrayPointer(inverse),
			SNI_GetRawArrayPointer(direction), &light, alpha);
		SNI_END_RAW_POINTERS
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(shaded);
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(javax_microedition_m3g_Rasterizer_draw0)
{
	struct target target;
	struct surface surface;
	int vertex_count = KNI_GetParameterAsInt(4);
	const char *error = NULL;

	KNI_StartHandles(7);
	KNI_DeclareHandle(pixels);
	KNI_DeclareHandle(view);
	KNI_DeclareHandle(vertices);
	KNI_DeclareHandle(strips);
	KNI_DeclareHandle(indices);
	KNI_DeclareHandle(texture);
	KNI_DeclareHandle(state);
	KNI_GetParameterAsObject(1, pixels);
	KNI_GetParameterAsObject(2, view);
	KNI_GetParameterAsObject(3, vertices);
	KNI_GetParameterAsObject(5, strips);
	KNI_GetParameterAsObject(6, indices);
	KNI_GetParameterAsObject(7, texture);
	KNI_GetParameterAsObject(8, state);

	if (!target_init(&target, pixels, view) || KNI_IsNullHandle(vertices) ||
	    KNI_IsNullHandle(strips) || KNI_IsNullHandle(indices) ||
	    KNI_IsNullHandle(state) ||
	    KNI_GetArrayLength(state) != STATE_FIELDS || vertex_count < 1 ||
	    vertex_count > KNI_GetArrayLength(vertices) / VERTEX_COMPONENTS) {
		error = "M3G buffers";
		goto done;
	}
	surface.state = SNI_GetRawArrayPointer(state);
	surface.texture = NULL;
	if (!KNI_IsNullHandle(texture)) {
		int width = surface.state[TEXTURE_WIDTH];
		int height = surface.state[TEXTURE_HEIGHT];

		if (width < 1 || height < 1 ||
		    (int64_t)width * height > KNI_GetArrayLength(texture)) {
			error = "M3G texture";
			goto done;
		}
		surface.texture = SNI_GetRawArrayPointer(texture);
	}
	if (!empty(&target.clip))
		depth_cleared = (struct rectangle){ 0 };
	if (!empty(&target.clip) &&
	    !draw_strips(&target, &surface, SNI_GetRawArrayPointer(vertices),
			 vertex_count, SNI_GetRawArrayPointer(strips),
			 KNI_GetArrayLength(strips),
			 SNI_GetRawArrayPointer(indices),
			 KNI_GetArrayLength(indices)))
		error = "M3G indices";

done:
	if (error)
		KNI_ThrowNew("java/lang/IllegalArgumentException", error);
	KNI_EndHandles();
	KNI_ReturnVoid();
}
