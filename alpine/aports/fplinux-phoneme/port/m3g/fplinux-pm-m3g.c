// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#include <kni.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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

static bool target_init(struct target *target, jarray pixels, jarray depths,
			jarray view)
{
	if (KNI_IsNullHandle(pixels) || KNI_IsNullHandle(depths) ||
	    KNI_IsNullHandle(view) || KNI_GetArrayLength(view) != VIEW_FIELDS)
		return false;

	const jint *bounds = SNI_GetRawArrayPointer(view);
	int width = bounds[VIEW_WIDTH], height = bounds[VIEW_HEIGHT];
	int x = bounds[VIEW_X], y = bounds[VIEW_Y];
	int viewport_width = bounds[VIEWPORT_WIDTH];
	int viewport_height = bounds[VIEWPORT_HEIGHT];
	int64_t count = (int64_t)width * height;
	int64_t right = (int64_t)x + viewport_width;
	int64_t bottom = (int64_t)y + viewport_height;

	if (width < 0 || height < 0 || viewport_width <= 0 ||
	    viewport_height <= 0 || count > KNI_GetArrayLength(pixels) ||
	    count > KNI_GetArrayLength(depths))
		return false;

	*target = (struct target){
		.pixels = SNI_GetRawArrayPointer(pixels),
		.depths = SNI_GetRawArrayPointer(depths),
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
	};
	return true;
}

static bool empty(const struct rectangle *clip)
{
	return clip->left >= clip->right || clip->top >= clip->bottom;
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

static uint32_t compose(int mode, uint32_t source, uint32_t destination)
{
	uint32_t rgb = 0;
	unsigned int alpha = channel(source, 24);

	if (mode == REPLACE)
		return source;

	for (unsigned int shift = 0; shift <= 24; shift += 8) {
		unsigned int from = channel(destination, shift);
		unsigned int to = channel(source, shift);
		unsigned int value;

		if (mode == ALPHA)
			value = mix(from, to, alpha);
		else if (mode == ALPHA_ADD)
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
		.z = values[Z] * inv_w * .5f + .5f,
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

static float state_float(jint value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

static void rasterize(struct target *target, const struct surface *surface,
		      const struct vertex *a, const struct vertex *b,
		      const struct vertex *c, const struct vertex *flat)
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
			float z = wa * pa.z + wb * pb.z + wc * pc.z + offset;

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

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(javax_microedition_m3g_Rasterizer_clear0)
{
	struct target target;
	jint color = KNI_GetParameterAsInt(4);
	jboolean color_clear = KNI_GetParameterAsBoolean(5);
	jboolean depth_clear = KNI_GetParameterAsBoolean(6);
	uint32_t alpha = KNI_GetParameterAsBoolean(9) ? 0xff000000U : 0;

	KNI_StartHandles(5);
	KNI_DeclareHandle(pixels);
	KNI_DeclareHandle(depths);
	KNI_DeclareHandle(view);
	KNI_DeclareHandle(image);
	KNI_DeclareHandle(crop);
	KNI_GetParameterAsObject(1, pixels);
	KNI_GetParameterAsObject(2, depths);
	KNI_GetParameterAsObject(3, view);
	KNI_GetParameterAsObject(7, image);
	KNI_GetParameterAsObject(8, crop);
	if (!target_init(&target, pixels, depths, view)) {
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

		for (int y = clip->top; y < clip->bottom; ++y) {
			jint *pixel_row = target.pixels + y * target.stride;
			jfloat *depth_row = target.depths + y * target.stride;
			int sy = source ? background_index(
						  y, layout[VIEW_Y],
						  layout[VIEWPORT_HEIGHT],
						  region[3], region[5],
						  region[1], region[7]) :
					  -1;

			/* Separate row loops let the compiler emit plain stores. */
			if (color_clear && sy < 0) {
				for (int x = clip->left; x < clip->right; ++x)
					pixel_row[x] = color;
			} else if (color_clear) {
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
			if (depth_clear) {
				for (int x = clip->left; x < clip->right; ++x)
					depth_row[x] = 1;
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
	float position[4], matrix[16], tex_matrix[16], tex_bias[3];

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
			float s = 0, t = 0;

			if (tex_input.values) {
				const jshort *coords = tex_input.values +
						       i * tex_input.components;

				s = coords[0] * tex_bias[0] + tex_bias[1];
				t = coords[1] * tex_bias[0] + tex_bias[2];
			}
			float u = transform_row(tex_matrix, s, t, 0);
			float v = transform_row(tex_matrix + 4, s, t, 0);
			float q = transform_row(tex_matrix + 12, s, t, 0);

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
	if (count < 1 || KNI_IsNullHandle(vertices) ||
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
			    KNI_GetArrayLength(tex_scale_bias) < 3)
				goto done;
			if (count > KNI_GetArrayLength(tex_values) /
					    tex_coords.components) {
				exception =
					"java/lang/ArrayIndexOutOfBoundsException";
				goto done;
			}
			tex_coords.values = SNI_GetRawArrayPointer(tex_values);
			tex_coords.scale_bias =
				SNI_GetRawArrayPointer(tex_scale_bias);
		}
	}
	positions.values = SNI_GetRawArrayPointer(position_values);
	positions.scale_bias = SNI_GetRawArrayPointer(position_scale_bias);
	prepare_vertices(SNI_GetRawArrayPointer(vertices), count, &positions,
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

	if (count > 0 && !KNI_IsNullHandle(vertices) &&
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
			SNI_GetRawArrayPointer(vertices), count,
			SNI_GetRawArrayPointer(normals),
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
	int vertex_count = KNI_GetParameterAsInt(5);
	const char *error = NULL;

	KNI_StartHandles(8);
	KNI_DeclareHandle(pixels);
	KNI_DeclareHandle(depths);
	KNI_DeclareHandle(view);
	KNI_DeclareHandle(vertices);
	KNI_DeclareHandle(strips);
	KNI_DeclareHandle(indices);
	KNI_DeclareHandle(texture);
	KNI_DeclareHandle(state);
	KNI_GetParameterAsObject(1, pixels);
	KNI_GetParameterAsObject(2, depths);
	KNI_GetParameterAsObject(3, view);
	KNI_GetParameterAsObject(4, vertices);
	KNI_GetParameterAsObject(6, strips);
	KNI_GetParameterAsObject(7, indices);
	KNI_GetParameterAsObject(8, texture);
	KNI_GetParameterAsObject(9, state);

	if (!target_init(&target, pixels, depths, view) ||
	    KNI_IsNullHandle(vertices) || KNI_IsNullHandle(strips) ||
	    KNI_IsNullHandle(indices) || KNI_IsNullHandle(state) ||
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
