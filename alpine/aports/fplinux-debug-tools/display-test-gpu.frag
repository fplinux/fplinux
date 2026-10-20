/* SPDX-License-Identifier: GPL-2.0-only */
#version 300 es
precision highp float;
precision highp int;

uniform int pattern;
uniform uint frame_id;
uniform int image_width;
uniform int image_height;
layout(location = 0) out vec4 output_color;

const int PATTERN_GEOMETRY = 0;
const int PATTERN_GRAY = 1;
const int PATTERN_IDS = 2;
const int PATTERN_MOTION = 3;
const uint GRAY = 0x8410u;
const uint DARK = 0x2104u;
const uint LIGHT = 0xdefbu;

uint fraction(uint extent, uint numerator, uint denominator)
{
	return extent * numerator / denominator;
}

vec4 rgb565(uint pixel)
{
	uint red = (pixel >> 11) & 31u;
	uint green = (pixel >> 5) & 63u;
	uint blue = pixel & 31u;
	uint red8 = (red << 3) | (red >> 2);
	uint green8 = (green << 2) | (green >> 4);
	uint blue8 = (blue << 3) | (blue >> 2);
	return vec4(vec3(float(red8), float(green8), float(blue8)) / 255.0,
		     1.0);
}

uint ids_pixel(ivec2 point, uint frame, uint pixel)
{
	uint width = uint(image_width);
	uint height = uint(image_height);
	uint cell_width = (width / 16u) & ~1u;
	uint left = (width - cell_width * 16u) / 2u;
	uint y = uint(point.y);
	uint x = uint(point.x);
	uint tops[3] = uint[3](height / 16u, fraction(height, 15u, 32u),
			       fraction(height, 7u, 8u));
	uint marker_height = height / 16u;

	if (cell_width == 0u)
		return pixel;
	for (uint row = 0u; row < 3u; ++row) {
		if (y < tops[row] || y >= tops[row] + marker_height)
			continue;
		if (x < left || x >= left + cell_width * 16u)
			continue;
		uint bit = (x - left) / cell_width;
		uint middle = left + bit * cell_width + cell_width / 2u;
		bool set = (frame & (1u << bit)) != 0u;
		bool first_half = x < middle;
		return (set == first_half) ? LIGHT : DARK;
	}
	return pixel;
}

uint scene_pixel(ivec2 point)
{
	uint width = uint(image_width);
	uint height = uint(image_height);
	uint x = uint(point.x);
	uint y = uint(point.y);
	uint pixel = pattern == PATTERN_MOTION ? DARK : GRAY;

	if (pattern == PATTERN_GEOMETRY) {
		uint left = width / 8u;
		uint right = fraction(width, 7u, 8u);
		uint top = height / 8u;
		uint bottom = fraction(height, 7u, 8u);
		uint second = top + (bottom - top) / 3u;
		uint third = top + fraction(bottom - top, 2u, 3u);
		if (x >= left && x < right && y >= top && y < bottom) {
			if (y < second) {
				uint colors[3] = uint[3](0xf800u, 0x07e0u, 0x001fu);
				pixel = colors[(x - left) % 3u];
			} else if (y < third) {
				uint colors[3] = uint[3](0xf800u, 0x07e0u, 0x001fu);
				pixel = colors[(y - second) % 3u];
			} else {
				pixel = ((x ^ y) & 1u) != 0u ? 0u : 0xffffu;
			}
		}
		if (y == 0u)
			pixel = 0xf800u;
		if (y == height - 1u)
			pixel = 0x001fu;
		if (x == 0u)
			pixel = 0x07e0u;
		if (x == width - 1u)
			pixel = 0xffffu;
		uint corner_width = width / 10u + 1u;
		uint corner_height = height / 10u + 1u;
		if (x < corner_width && y < corner_height)
			pixel = 0xf800u;
		if (x >= width - corner_width && y < corner_height)
			pixel = 0x07e0u;
		if (x < corner_width && y >= height - corner_height)
			pixel = 0x001fu;
		if (x >= width - corner_width && y >= height - corner_height)
			pixel = 0xffe0u;
	} else if (pattern == PATTERN_IDS || pattern == PATTERN_MOTION) {
		if (pattern == PATTERN_MOTION &&
	    x < ((frame_id % width) * 8u) % width)
			pixel = LIGHT;
		pixel = ids_pixel(point, frame_id, pixel);
	}
	if (pattern != PATTERN_GRAY && x >= fraction(width, 3u, 4u) &&
	    x < fraction(width, 15u, 16u) && y >= height / 4u &&
	    y < fraction(height, 3u, 8u))
		pixel = GRAY;
	return pixel;
}

void main()
{
	ivec2 point = ivec2(gl_FragCoord.xy);
	point.y = image_height - 1 - point.y;
	output_color = rgb565(scene_pixel(point));
}
