// SPDX-License-Identifier: GPL-2.0-only
#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

in vec3 v_world;
flat in vec3 v_midpoint;
flat in vec3 v_normal;
layout(location = 0) out vec4 fragment;

uniform bool u_reflection;
uniform vec2 u_size;
uniform vec2 u_view_size;
uniform vec4 u_refine;
uniform uint u_frame;
uniform vec3 u_color;
uniform float u_emissive;
uniform float u_letter_scale;
uniform float u_reflection_visibility;
uniform vec3 u_light_color;
uniform vec3 u_celestial;
uniform vec4 u_sun;
uniform vec4 u_moon;
uniform vec3 u_environment;
uniform float u_wave[64];
uniform int u_ripple_count;
uniform vec4 u_ripples[7];
uniform sampler2D u_source;
uniform sampler2D u_shadow;
uniform vec3 u_shadow_right;
uniform vec3 u_shadow_up;
uniform vec3 u_shadow_direction;
uniform vec4 u_shadow_transform;
uniform vec2 u_shadow_depth_range;
uniform vec4 u_shadow_bounds[7];
uniform float u_shadow_max_depth[7];
uniform vec3 u_shadow_filters[7];
uniform vec2 u_shadow_slopes[12];
uniform vec2 u_shadow_max_slope;
uniform float u_shadow_strength;

#if ARMADA_PASS == 0 || ARMADA_PASS == 1
const vec3 camera = vec3(0.0, 0.75, -1.35);

float smooth_unit(float value)
{
	value = clamp(value, 0.0, 1.0);
	return value * value * (3.0 - 2.0 * value);
}

uint hash32(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	return value ^ (value >> 16);
}

float noise_corner(ivec2 cell)
{
	uint hash = hash32(uint(cell.x) * 0x68bc21ebu ^ uint(cell.y) * 0x02e5be93u);
	return float(hash & 0xffffu) / 65535.0;
}

float value_noise(vec2 point)
{
	ivec2 cell = ivec2(floor(point));
	vec2 fraction = fract(point);
	fraction = fraction * fraction * (3.0 - 2.0 * fraction);
	return mix(mix(noise_corner(cell), noise_corner(cell + ivec2(1, 0)), fraction.x),
		mix(noise_corner(cell + ivec2(0, 1)), noise_corner(cell + ivec2(1, 1)), fraction.x), fraction.y);
}

float cloud_plane_coverage(vec2 point)
{
	float drift = float(u_frame) * 0.0019;
	vec2 wind = point * vec2(0.22, 0.16) + drift * vec2(1.0, 0.48);
	float field = value_noise(wind) * 0.72 + value_noise(wind * vec2(2.03, 2.07) + vec2(7.3, -3.1)) * 0.28;
	return clamp(smooth_unit((field - 0.54) / 0.20) * 0.68, 0.0, 0.68);
}

float cloud_coverage_ray(vec3 origin, vec3 direction)
{
	float travel = (7.2 - origin.y) / max(direction.y, 0.12);
	return cloud_plane_coverage((origin + direction * travel).xz) * smooth_unit((direction.y - 0.015) / 0.17);
}

float cloud_transmittance()
{
	return 1.0 - cloud_coverage_ray(camera, u_celestial) * 0.20;
}

float surface_cloud_coverage(vec3 point)
{
	float weight = smooth_unit((u_celestial.y - 0.04) / 0.20);
	vec3 projected = point + u_celestial * ((7.2 - point.y) / max(u_celestial.y, 0.12));
	float global = cloud_coverage_ray(camera, u_celestial);
	return mix(global, cloud_plane_coverage(projected.xz), weight);
}

vec3 star_color(vec3 direction)
{
	if (direction.y <= 0.0)
		return vec3(0.0);
	ivec2 cell = ivec2(floor((direction.xy / max(direction.z, 0.05) + vec2(1.8, 0.2)) * 96.0));
	uint hash = hash32(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u);
	if ((hash & 511u) >= 3u)
		return vec3(0.0);
	uint twinkle = (u_frame / 18u + (hash >> 11)) & 3u;
	return vec3(0.62, 0.82, 1.0) * (0.06 + u_environment.y * 0.94) * (0.30 + float(twinkle) * 0.14);
}

float celestial_edge(vec3 direction, vec3 body, float radius)
{
	vec2 difference = direction.xy / max(direction.z, 0.05) - body.xy / max(body.z, 0.05);
	float inner = max(radius - 2.0 / (u_size.y * 0.95), radius * 0.55);
	return smooth_unit((radius * radius - dot(difference, difference)) / (radius * radius - inner * inner));
}

vec3 sun_color()
{
	return mix(vec3(1.0, 0.34, 0.08), vec3(1.0, 0.97, 0.76), smooth_unit(u_sun.y / 0.42));
}

vec3 sky_color(vec3 origin, vec3 direction, vec3 transmission)
{
	float height = clamp(direction.y * 1.55 + 0.30, 0.0, 1.0);
	vec3 day = mix(vec3(0.24, 0.62, 0.82), vec3(0.035, 0.26, 0.68), height);
	vec3 night = mix(vec3(0.018, 0.055, 0.13), vec3(0.002, 0.008, 0.038), height);
	vec3 color = mix(night, day, clamp(u_environment.x, 0.0, 1.0));
	float twilight = clamp(1.0 - u_environment.x - u_environment.y * 0.35, 0.0, 1.0);
	float sun_facing = dot(direction, u_sun.xyz);
	color = mix(color, vec3(0.44, 0.105, 0.065), twilight * (1.0 - height) * 0.72);
	color += (sun_color() - color) * celestial_edge(direction, u_sun.xyz, 0.046) * u_sun.w * transmission;
	color += (vec3(0.68, 0.80, 1.0) - color) * celestial_edge(direction, u_moon.xyz, 0.040) * u_moon.w * transmission;
	color += star_color(direction);
	float light_facing = smooth_unit((dot(direction, u_celestial) - 0.72) / 0.28);
	float warm_edge = twilight * smooth_unit((sun_facing - 0.90) / 0.10);
	vec3 day_cloud = mix(vec3(0.54, 0.64, 0.72), vec3(0.92, 0.95, 0.97), light_facing);
	vec3 night_cloud = mix(vec3(0.060, 0.100, 0.180), vec3(0.30, 0.38, 0.55), light_facing * 0.80);
	vec3 clouds = mix(night_cloud, day_cloud, u_environment.x);
	clouds = mix(clouds, vec3(0.90, 0.34, 0.16), warm_edge * 0.46);
	return mix(color, clouds, cloud_coverage_ray(origin, direction) * 0.68);
}

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

bool water_intersection(vec3 direction, out float travel)
{
	if (direction.y >= -0.0001)
		return false;
	travel = (-0.72 - camera.y) / direction.y;
	for (int iteration = 0; iteration < 2; ++iteration) {
		vec3 current = camera + direction * travel;
		vec3 water = water_sample(current.xz);
		float derivative = direction.y - dot(water.yz, direction.xz);
		if (abs(derivative) < 0.025)
			derivative = derivative < 0.0 ? -0.025 : 0.025;
		travel -= (current.y - water.x) / derivative;
	}
	return travel > 0.0 && travel < 80.0;
}

vec3 water_normal(vec3 point, vec3 water, float distance)
{
	float distance_fade = 1.0 - smooth_unit((distance - 13.0) / 34.0) * 0.88;
	float ripple_fade = 1.0 - smooth_unit((distance - 13.0) / 19.0) * 0.96;
	float first = wave(point.x * 18.0 + point.z * 11.0 + float(u_frame) * 0.76 + 16.0);
	float second = wave(point.x * -15.0 + point.z * 21.0 - float(u_frame) * 0.63 + 16.0);
	vec2 gradient = water.yz * distance_fade + vec2(first * 0.032 - second * 0.021, first * 0.020 + second * 0.030) * ripple_fade;
	return normalize(vec3(-gradient.x, 1.0, -gradient.y));
}

vec2 shadow_sample(int letter, vec2 point, float receiver)
{
	ivec2 first = ivec2(floor(point));
	vec2 fraction = fract(point);
	vec2 result = vec2(0.0);
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 2; ++x) {
			ivec2 texel = first + ivec2(x, y);
			if (texel.x < 0 || texel.y < 0 || texel.x >= 128 || texel.y >= 128)
				continue;
			vec4 encoded_depth = texelFetch(u_shadow, texel + ivec2(letter * 128, 0), 0);
			if (encoded_depth.a < 0.5)
				continue;
			uvec3 bytes = uvec3(encoded_depth.rgb * 255.0 + 0.5);
			uint bits = (bytes.r << 16) | (bytes.g << 8) | bytes.b;
			float depth = u_shadow_depth_range.x + float(bits) / 16777215.0 * u_shadow_depth_range.y;
			if (depth <= receiver)
				continue;
			float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
			result += vec2(1.0, depth - receiver) * weight;
		}
	}
	return result;
}

float shadow_letter_coverage(int letter, vec2 point, float receiver)
{
	float maximum_separation = u_shadow_max_depth[letter] - receiver;
	if (maximum_separation <= 0.0)
		return 0.0;
	vec2 radius = maximum_separation * u_shadow_max_slope * u_shadow_transform.zw;
	vec4 bounds = u_shadow_bounds[letter];
	if (point.x + radius.x < bounds.x - 1.0 || point.x - radius.x > bounds.y + 1.0 ||
		point.y + radius.y < bounds.z - 1.0 || point.y - radius.y > bounds.w + 1.0)
		return 0.0;
	if (radius.x < 0.5 && radius.y < 0.5)
		return shadow_sample(letter, point, receiver).x;
	vec2 blocker = vec2(0.0);
	for (int sample_index = 0; sample_index < 12; ++sample_index)
		blocker += shadow_sample(letter, point + maximum_separation * u_shadow_slopes[sample_index] * u_shadow_transform.zw, receiver);
	if (blocker.x <= 0.0)
		return 0.0;
	float separation = blocker.y / blocker.x;
	float coverage = 0.0;
	for (int sample_index = 0; sample_index < 12; ++sample_index)
		coverage += shadow_sample(letter, point + separation * u_shadow_slopes[sample_index] * u_shadow_transform.zw, receiver).x;
	return coverage / 12.0;
}

vec3 water_light_transmission(vec3 point)
{
	vec3 transmission = vec3(1.0);
	if (u_shadow_strength <= 0.0)
		return transmission;
	vec2 projected = (vec2(dot(point, u_shadow_right), dot(point, u_shadow_up)) - u_shadow_transform.xy) * u_shadow_transform.zw - 0.5;
	float receiver = dot(point, u_shadow_direction) + 0.005;
	float strength = u_shadow_strength * cloud_transmittance();
	for (int letter = 0; letter < 7; ++letter) {
		float coverage = shadow_letter_coverage(letter, projected, receiver);
		transmission *= mix(vec3(1.0), u_shadow_filters[letter], coverage * strength);
	}
	return transmission;
}

vec3 water_glint(vec3 reflected, vec4 body, vec3 color, float cloud, float intensity)
{
	if (body.w <= 0.0 || body.y <= 0.0)
		return vec3(0.0);
	vec2 distance = abs(reflected.xy / max(reflected.z, 0.05) - body.xy / max(body.z, 0.05));
	float across = smooth_unit(1.0 - distance.x / 0.075);
	float along = smooth_unit(1.0 - distance.y / 0.38);
	return color * body.w * cloud * across * across * along * intensity;
}

vec3 background(vec3 direction)
{
	float distance;
	if (!water_intersection(direction, distance))
		return sky_color(camera, direction, vec3(1.0)) * u_environment.z;
	vec3 point = camera + direction * distance;
	vec3 water = water_sample(point.xz);
	vec3 transmission = water_light_transmission(point);
	vec3 normal = water_normal(point, water, distance);
	vec3 reflected = reflect(direction, normal);
	float grazing = 1.0 - clamp(dot(normal, -direction), 0.0, 1.0);
	float fresnel = 0.040 + 0.960 * grazing * grazing * grazing * grazing * grazing;
	vec3 deep = mix(vec3(0.002, 0.022, 0.060), vec3(0.005, 0.09, 0.18), u_environment.x);
	float cloud = surface_cloud_coverage(point);
	vec3 sky = sky_color(point, reflected, transmission);
	vec3 glint = water_glint(reflected, u_sun, sun_color(), 1.0 - cloud, 0.30) +
		water_glint(reflected, u_moon, vec3(0.68, 0.80, 1.0), 1.0 - cloud, 0.16);
	vec3 base;
	if (distance > 13.0) {
		float haze = clamp((distance - 13.0) / 67.0, 0.0, 1.0);
		base = mix(deep, sky, haze * 0.32);
	} else {
		vec3 near_water = mix(vec3(0.012, 0.125, 0.16), vec3(0.025, 0.30, 0.36), u_environment.x);
		base = mix(near_water, deep, clamp(distance / 80.0, 0.0, 1.0) * 0.82);
		float crest = smooth_unit((water.x + 0.72 + 0.008) / 0.070);
		float steepness = smooth_unit((abs(water.y) + abs(water.z) - 0.018) / 0.075);
		vec3 crest_color = mix(vec3(0.025, 0.20, 0.29), vec3(0.16, 0.58, 0.62), u_environment.x);
		base = mix(base, crest_color, crest * steepness * (0.07 + u_environment.x * 0.07));
		base *= 1.0 - cloud * 0.16;
	}
	base *= mix(vec3(1.0), transmission, 0.65);
	return (mix(base, sky, fresnel) + glint * transmission) * u_environment.z;
}

vec3 output_ray(vec2 pixel, vec2 size)
{
	return normalize(vec3((2.0 * pixel.x - size.x) / (size.y * 0.95),
		(size.y * 1.125 - 2.0 * (size.y - pixel.y)) / (size.y * 0.95), 1.0));
}

vec3 glass_color(out vec2 offset)
{
	vec3 normal = normalize(v_normal);
	vec3 point = v_midpoint;
	vec3 relative = point - camera;
	vec3 incident = normalize(relative);
	float facing = clamp(-dot(normal, incident), 0.0, 1.0);
	float eta = 1.0 / 1.50;
	float transmitted_cosine = sqrt(1.0 - eta * eta * (1.0 - facing * facing));
	vec3 through = incident * eta + normal * (eta * facing - transmitted_cosine);
	vec3 exit_point = relative + through * (0.34 * u_letter_scale / transmitted_cosine);
	offset = u_size.y * 0.475 * (exit_point.xy / exit_point.z - relative.xy / relative.z);
	float rim = 1.0 - facing;
	float rim_squared = rim * rim;
	float reflectance = 0.04 + 0.96 * rim_squared * rim_squared * rim;
	vec3 view = -incident;
	vec3 reflected = reflect(incident, normal);
	if (u_reflection) {
		point.y = -1.44 - point.y;
		normal.y = -normal.y;
		view.y = -view.y;
		reflected.y = -reflected.y;
	}
	vec3 half_vector = normalize(u_celestial + view);
	float diffuse = max(dot(normal, u_celestial), 0.0);
	float highlight = max(dot(normal, half_vector), 0.0);
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	highlight *= highlight;
	vec3 environment = reflected.y > 0.0 ? sky_color(point, reflected, vec3(1.0)) :
		mix(vec3(0.008, 0.045, 0.075), vec3(0.055, 0.24, 0.30), u_environment.x);
	float scatter = (0.80 + 0.18 * u_environment.x + diffuse * 0.12) * (1.0 - reflectance);
	vec3 light = environment * reflectance + u_color * scatter;
	light += u_color * rim_squared * (0.18 + u_environment.y * 0.15);
	light += u_light_color * highlight * 0.38 * cloud_transmittance();
	light += mix(u_color, vec3(1.0), 0.65) * u_emissive * (0.26 + rim * 0.55);
	vec3 transmission = mix(vec3(1.0), u_color, 0.72 + rim * 0.20) * (1.0 - reflectance) * 0.78;
	vec2 uv = clamp((gl_FragCoord.xy + offset) / u_size, 0.5 / u_size, 1.0 - 0.5 / u_size);
	return light * u_environment.z + texture(u_source, uv).rgb * transmission;
}
#endif

void main()
{
#if ARMADA_PASS == 2
	float depth = clamp((dot(v_world, u_shadow_direction) - u_shadow_depth_range.x) / u_shadow_depth_range.y, 0.0, 1.0);
	uint bits = uint(depth * 16777215.0 + 0.5);
	fragment = vec4(vec3(float((bits >> 16) & 255u), float((bits >> 8) & 255u), float(bits & 255u)) / 255.0, 1.0);
#elif ARMADA_PASS == 3
	fragment = vec4(texture(u_source, gl_FragCoord.xy / u_size).rgb, 1.0);
#elif ARMADA_PASS == 0
	vec3 direction = output_ray(gl_FragCoord.xy, u_view_size);
	vec2 difference = direction.xy / max(direction.z, 0.05) - u_refine.xy / max(u_refine.z, 0.05);
	if (u_refine.w > 0.0 && dot(difference, difference) > u_refine.w * u_refine.w)
		discard;
	fragment = vec4(background(direction), 1.0);
#else
	if (dot(v_normal, camera - v_midpoint) <= 0.0)
		discard;
	vec3 ray = output_ray(gl_FragCoord.xy, u_size);
	float water_distance;
	bool water = water_intersection(ray, water_distance);
	if (u_reflection ? !water : (water && (v_world.z - camera.z) > water_distance * ray.z))
		discard;
	vec2 offset;
	vec3 color = glass_color(offset);
	if (u_reflection) {
		float alpha = (0.10 + 0.24 * u_environment.x) * u_reflection_visibility;
		color = mix(texture(u_source, gl_FragCoord.xy / u_size).rgb, color * 0.58, alpha);
	}
	fragment = vec4(color, 1.0);
#endif
}
