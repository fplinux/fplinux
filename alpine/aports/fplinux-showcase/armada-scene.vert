// SPDX-License-Identifier: GPL-2.0-only
#version 300 es
precision highp float;
precision highp int;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_midpoint;
layout(location = 2) in vec3 a_normal;

uniform bool u_reflection;
uniform vec2 u_size;
uniform uint u_frame;
uniform mat3 u_rotation;
uniform vec3 u_scale;
uniform vec3 u_position;
uniform float u_wave[64];
uniform int u_ripple_count;
uniform vec4 u_ripples[7];
uniform vec3 u_shadow_right;
uniform vec3 u_shadow_up;
uniform vec3 u_shadow_direction;
uniform vec4 u_shadow_transform;
uniform vec2 u_shadow_depth_range;

out vec3 v_world;
flat out vec3 v_midpoint;
flat out vec3 v_normal;

#if ARMADA_PASS == 1
float wave(float phase)
{
	int first = int(floor(phase));
	return mix(u_wave[first & 63], u_wave[(first + 1) & 63], fract(phase));
}

vec3 water_sample(vec2 point)
{
	float time = float(u_frame);
	vec3 phase = vec3(point.x * 6.0 + point.y * 3.0 + time * 0.36,
		point.x * -4.0 + point.y * 8.0 - time * 0.29,
		point.x * 10.0 - point.y * 5.0 + time * 0.48);
	vec3 cosine = vec3(wave(phase.x + 16.0), wave(phase.y + 16.0), wave(phase.z + 16.0));
	vec3 result = vec3(-0.72 + wave(phase.x) * 0.035 + wave(phase.y) * 0.026 + wave(phase.z) * 0.019,
		dot(cosine, vec3(0.035 * 6.0, 0.026 * -4.0, 0.019 * 10.0)) * 0.09817477,
		dot(cosine, vec3(0.035 * 3.0, 0.026 * 8.0, 0.019 * -5.0)) * 0.09817477);
	for (int index = 0; index < u_ripple_count; ++index) {
		vec4 ripple = u_ripples[index];
		vec2 offset = point - ripple.xy;
		float band = 2.0 * ripple.z * 0.30 + 0.09;
		float q = (dot(offset, offset) - ripple.z * ripple.z) / band;
		if (abs(q) >= 1.0)
			continue;
		float envelope = 1.0 - q * q;
		result.x += ripple.w * (1.0 - 4.0 * q * q) * envelope * envelope;
		result.yz += ripple.w * -12.0 * q * envelope * (1.0 - 2.0 * q * q) * 2.0 * offset / band;
	}
	return result;
}
#endif

void main()
{
#if ARMADA_PASS == 0 || ARMADA_PASS == 3
	vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
	v_world = vec3(0.0);
	v_midpoint = vec3(0.0);
	v_normal = vec3(0.0, 1.0, 0.0);
#else
	vec3 world = u_position + u_rotation * (a_position * u_scale);
#if ARMADA_PASS == 2
	vec2 light = vec2(dot(world, u_shadow_right), dot(world, u_shadow_up));
	vec2 projected = (light - u_shadow_transform.xy) * u_shadow_transform.zw;
	float depth = (dot(world, u_shadow_direction) - u_shadow_depth_range.x) / u_shadow_depth_range.y;
	gl_Position = vec4(projected / 64.0 - 1.0, 1.0 - 2.0 * depth, 1.0);
	v_world = world;
#else
	v_midpoint = u_position + u_rotation * (a_midpoint * u_scale);
	v_normal = normalize(u_rotation * (a_normal / u_scale));
	if (u_reflection) {
		world.y = -1.44 - world.y;
		v_midpoint.y = -1.44 - v_midpoint.y;
		v_normal.y = -v_normal.y;
	}
	v_world = world;
	vec3 relative = world - vec3(0.0, 0.75, -1.35);
	vec2 clip = vec2(relative.x * u_size.y * 0.95 / u_size.x - relative.z / u_size.x,
		relative.y * 0.95 + (-0.125 + 1.0 / u_size.y) * relative.z);
	if (u_reflection) {
		vec3 water = water_sample(world.xz);
		vec2 offset = vec2(water.y * 14.0 + water.z * 3.0,
			(water.x + 0.72) * 9.0 + water.z * 5.0);
		clip += vec2(2.0, -2.0) * offset / u_size * relative.z;
	}
	gl_Position = vec4(clip, (100.05 / 99.95) * relative.z - 10.0 / 99.95, relative.z);
#endif
#endif
}
