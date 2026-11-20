/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-rgb565.h"

#include <stdint.h>
#include <string.h>

#define FPLINUX_RGB565_MAX_DIMENSION 4096

static int min_int(int a, int b)
{
	return a < b ? a : b;
}

static int max_int(int a, int b)
{
	return a > b ? a : b;
}

static int centered_offset(int outer, int inner)
{
	return outer >= inner ? (outer - inner) / 2 :
				-((inner - outer + 1) / 2);
}

static int dimension_valid(int value)
{
	return value > 0 && value <= FPLINUX_RGB565_MAX_DIMENSION;
}

int fplinux_rgb565_viewport_init(struct fplinux_rgb565_viewport *viewport,
				 int source_width, int source_height,
				 int destination_width, int destination_height,
				 enum fplinux_rgb565_scale scale,
				 enum fplinux_rgb565_rotation rotation)
{
	struct fplinux_rgb565_viewport result;
	int width, height, factor;

	if (!viewport || !dimension_valid(source_width) ||
	    !dimension_valid(source_height) ||
	    !dimension_valid(destination_width) ||
	    !dimension_valid(destination_height) ||
	    (rotation != FPLINUX_RGB565_ROTATE_0 &&
	     rotation != FPLINUX_RGB565_ROTATE_90 &&
	     rotation != FPLINUX_RGB565_ROTATE_270) ||
	    (scale != FPLINUX_RGB565_FIT &&
	     scale != FPLINUX_RGB565_ONE_TO_ONE &&
	     scale != FPLINUX_RGB565_INTEGER))
		return -1;

	width = rotation == FPLINUX_RGB565_ROTATE_0 ? source_width :
						      source_height;
	height = rotation == FPLINUX_RGB565_ROTATE_0 ? source_height :
						       source_width;
	if (scale == FPLINUX_RGB565_FIT) {
		if (destination_width * height <= destination_height * width) {
			height = max_int(1, height * destination_width / width);
			width = destination_width;
		} else {
			width = max_int(1, width * destination_height / height);
			height = destination_height;
		}
	} else if (scale == FPLINUX_RGB565_INTEGER) {
		if (width <= destination_width &&
		    height <= destination_height) {
			factor = min_int(destination_width / width,
					 destination_height / height);
			width *= factor;
			height *= factor;
		} else {
			factor = max_int((width + destination_width - 1) /
						 destination_width,
					 (height + destination_height - 1) /
						 destination_height);
			width = max_int(1, width / factor);
			height = max_int(1, height / factor);
		}
	}
	result = (struct fplinux_rgb565_viewport){
		.source_width = source_width,
		.source_height = source_height,
		.destination_width = destination_width,
		.destination_height = destination_height,
		.x = centered_offset(destination_width, width),
		.y = centered_offset(destination_height, height),
		.width = width,
		.height = height,
		.rotation = rotation,
	};
	*viewport = result;
	return 0;
}

static int viewport_valid(const struct fplinux_rgb565_viewport *v)
{
	return v && dimension_valid(v->source_width) &&
	       dimension_valid(v->source_height) &&
	       dimension_valid(v->destination_width) &&
	       dimension_valid(v->destination_height) &&
	       dimension_valid(v->width) && dimension_valid(v->height) &&
	       v->x == centered_offset(v->destination_width, v->width) &&
	       v->y == centered_offset(v->destination_height, v->height) &&
	       (v->rotation == FPLINUX_RGB565_ROTATE_0 ||
		v->rotation == FPLINUX_RGB565_ROTATE_90 ||
		v->rotation == FPLINUX_RGB565_ROTATE_270);
}

static void source_point(const struct fplinux_rgb565_viewport *v, int x, int y,
			 int *source_x, int *source_y)
{
	if (v->rotation == FPLINUX_RGB565_ROTATE_90) {
		*source_x = y;
		*source_y = v->source_height - 1 - x;
	} else if (v->rotation == FPLINUX_RGB565_ROTATE_270) {
		*source_x = v->source_width - 1 - y;
		*source_y = x;
	} else {
		*source_x = x;
		*source_y = y;
	}
}

int fplinux_rgb565_map_point(const struct fplinux_rgb565_viewport *v, int x,
			     int y, int *source_x, int *source_y)
{
	int rotated_width, rotated_height;

	if (!viewport_valid(v) || !source_x || !source_y)
		return -1;
	if (x < 0 || y < 0 || x >= v->destination_width ||
	    y >= v->destination_height || x < v->x || y < v->y ||
	    x >= v->x + v->width || y >= v->y + v->height)
		return 0;
	rotated_width = v->rotation == FPLINUX_RGB565_ROTATE_0 ?
				v->source_width :
				v->source_height;
	rotated_height = v->rotation == FPLINUX_RGB565_ROTATE_0 ?
				 v->source_height :
				 v->source_width;
	source_point(v, (x - v->x) * rotated_width / v->width,
		     (y - v->y) * rotated_height / v->height, source_x,
		     source_y);
	return 1;
}

struct axis_span {
	int first, last;
	int start, end, unit;
};

static struct axis_span span_for(int position, int source, int destination)
{
	struct axis_span span;

	if (source <= destination) {
		span.first = position * source / destination;
		span.last = span.first + 1;
		span.start = span.first;
		span.end = span.last;
		span.unit = 1;
	} else {
		span.start = position * source;
		span.end = (position + 1) * source;
		span.first = span.start / destination;
		span.last = (span.end + destination - 1) / destination;
		span.unit = destination;
	}
	return span;
}

static int span_weight(const struct axis_span *span, int index)
{
	return min_int(span->end, (index + 1) * span->unit) -
	       max_int(span->start, index * span->unit);
}

static uint16_t sample(const struct fplinux_rgb565_viewport *v,
		       const uint16_t *source, size_t stride, int x, int y)
{
	int rotated_width = v->rotation == FPLINUX_RGB565_ROTATE_0 ?
				    v->source_width :
				    v->source_height;
	int rotated_height = v->rotation == FPLINUX_RGB565_ROTATE_0 ?
				     v->source_height :
				     v->source_width;
	struct axis_span xs = span_for(x, rotated_width, v->width);
	struct axis_span ys = span_for(y, rotated_height, v->height);
	/* At most 4096 * 4096 weight units, each channel at most 63. */
	uint32_t red = 0, green = 0, blue = 0;
	/* A reduced axis has total weight equal to its source dimension. */
	const uint32_t total =
		(uint32_t)(rotated_width > v->width ? rotated_width : 1) *
		(uint32_t)(rotated_height > v->height ? rotated_height : 1);
	int source_x, source_y;

	if (rotated_width <= v->width && rotated_height <= v->height) {
		source_point(v, xs.first, ys.first, &source_x, &source_y);
		return source[(size_t)source_y * stride + source_x];
	}

	for (int sy = ys.first; sy < ys.last; ++sy) {
		for (int sx = xs.first; sx < xs.last; ++sx) {
			int source_x, source_y;
			uint32_t weight =
				span_weight(&xs, sx) * span_weight(&ys, sy);
			uint16_t pixel;

			source_point(v, sx, sy, &source_x, &source_y);
			pixel = source[(size_t)source_y * stride + source_x];
			red += ((pixel >> 11) & 31) * weight;
			green += ((pixel >> 5) & 63) * weight;
			blue += (pixel & 31) * weight;
		}
	}
	return (uint16_t)(((red + total / 2) / total << 11) |
			  ((green + total / 2) / total << 5) |
			  ((blue + total / 2) / total));
}

static uint16_t average_2x2(const uint16_t *upper, const uint16_t *lower)
{
	uint16_t a = upper[0], b = upper[1], c = lower[0], d = lower[1];
	unsigned int red = (a >> 11) + (b >> 11) + (c >> 11) + (d >> 11);
	unsigned int green = ((a >> 5) & 63) + ((b >> 5) & 63) +
			     ((c >> 5) & 63) + ((d >> 5) & 63);
	unsigned int blue = (a & 31) + (b & 31) + (c & 31) + (d & 31);

	/* Equal pixel weights cancel, leaving a rounded mean per channel. */
	return (uint16_t)(((red + 2) / 4 << 11) | ((green + 2) / 4 << 5) |
			  ((blue + 2) / 4));
}

static int stride_valid(size_t stride, int width, int height)
{
	return stride >= (size_t)width &&
	       (height == 1 ||
		stride <= ((size_t)PTRDIFF_MAX / sizeof(uint16_t) - width) /
				  (size_t)(height - 1));
}

int fplinux_rgb565_transform(const struct fplinux_rgb565_viewport *v,
			     const uint16_t *source, size_t source_stride,
			     uint16_t *destination, size_t destination_stride)
{
	int left, top, right, bottom;

	if (!viewport_valid(v) || !source || !destination ||
	    !stride_valid(source_stride, v->source_width, v->source_height) ||
	    !stride_valid(destination_stride, v->destination_width,
			  v->destination_height))
		return -1;
	left = max_int(0, v->x);
	top = max_int(0, v->y);
	right = min_int(v->destination_width, v->x + v->width);
	bottom = min_int(v->destination_height, v->y + v->height);
	/* Native and original-size frames need no per-pixel coordinate mapping. */
	if (v->rotation == FPLINUX_RGB565_ROTATE_0 &&
	    v->width == v->source_width && v->height == v->source_height) {
		for (int y = top; y < bottom; ++y)
			memcpy(destination + (size_t)y * destination_stride +
				       left,
			       source + (size_t)(y - v->y) * source_stride +
				       (left - v->x),
			       (size_t)(right - left) * sizeof(*source));
		return 0;
	}
	if (v->rotation == FPLINUX_RGB565_ROTATE_0 &&
	    v->width * 2 == v->source_width &&
	    v->height * 2 == v->source_height) {
		for (int y = top; y < bottom; ++y) {
			int source_y = 2 * (y - v->y);
			const uint16_t *upper =
				source + (size_t)source_y * source_stride;
			const uint16_t *lower = upper + source_stride;
			uint16_t *row =
				destination + (size_t)y * destination_stride;

			for (int x = left; x < right; ++x) {
				int source_x = 2 * (x - v->x);

				row[x] = average_2x2(upper + source_x,
						     lower + source_x);
			}
		}
		return 0;
	}
	for (int y = top; y < bottom; ++y)
		for (int x = left; x < right; ++x)
			destination[(size_t)y * destination_stride + x] =
				sample(v, source, source_stride, x - v->x,
				       y - v->y);
	return 0;
}
