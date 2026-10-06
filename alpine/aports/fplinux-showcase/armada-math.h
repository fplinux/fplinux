// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_ARMADA_MATH_H
#define FPLINUX_ARMADA_MATH_H

#include "armada-frame.h"

#include <string.h>

static inline float f_abs(float value)
{
	return value < 0.0f ? -value : value;
}
static inline float f_min(float first, float second)
{
	return first < second ? first : second;
}
static inline float f_max(float first, float second)
{
	return first > second ? first : second;
}
static inline float f_clamp(float value, float minimum, float maximum)
{
	return f_min(f_max(value, minimum), maximum);
}
static inline float smoothstep(float value)
{
	value = f_clamp(value, 0.0f, 1.0f);
	return value * value * (3.0f - 2.0f * value);
}

static inline struct armada_vec3 vec3(float x, float y, float z)
{
	struct armada_vec3 result = { .x = x, .y = y, .z = z };
	return result;
}
static inline struct armada_vec3 vec_add(struct armada_vec3 a,
					 struct armada_vec3 b)
{
	return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline struct armada_vec3 vec_sub(struct armada_vec3 a,
					 struct armada_vec3 b)
{
	return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline struct armada_vec3 vec_scale(struct armada_vec3 v, float s)
{
	return vec3(v.x * s, v.y * s, v.z * s);
}
static inline float vec_dot(struct armada_vec3 a, struct armada_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline float fast_inverse_sqrt(float value)
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
static inline struct armada_vec3 vec_normalize(struct armada_vec3 v)
{
	float inverse = fast_inverse_sqrt(vec_dot(v, v));
	return inverse > 0.0f ? vec_scale(v, inverse) : vec3(0.0f, 1.0f, 0.0f);
}
static inline struct armada_vec3 color_mix(struct armada_vec3 a,
					   struct armada_vec3 b, float weight)
{
	weight = f_clamp(weight, 0.0f, 1.0f);
	return vec_add(vec_scale(a, 1.0f - weight), vec_scale(b, weight));
}
static inline float wave_sample(int phase)
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

static inline float wave_sample_f(float phase)
{
	int first = (int)phase;
	float fraction;

	if (phase < (float)first)
		--first;
	fraction = phase - (float)first;
	return wave_sample(first) * (1.0f - fraction) +
	       wave_sample(first + 1) * fraction;
}
static inline struct armada_vec3 camera_position(void)
{
	return vec3(0.0f, 0.75f, -1.35f);
}

#endif
