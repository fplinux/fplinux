// SPDX-License-Identifier: GPL-2.0-only
/* Independent host oracle for the production CPU rotation reference. */
#include "fplinux-rotate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 37U
#define GUARD_VALUE 0xd3U
#define DESTINATION_VALUE 0xa5U

struct guarded_image {
	struct fplinux_rotate_image image;
	uint8_t *allocation[2];
};

struct transform_case {
	unsigned int rotation;
	int mirror;
};

static int guarded_allocate(struct guarded_image *guarded,
			    enum fplinux_rotate_format format, uint32_t width,
			    uint32_t height, uint32_t padding)
{
	unsigned int plane;

	memset(guarded, 0, sizeof(*guarded));
	guarded->image.format = format;
	guarded->image.width = width;
	guarded->image.height = height;
	guarded->image.planes = fplinux_rotate_plane_count(format);
	for (plane = 0; plane < guarded->image.planes; ++plane) {
		uint32_t rows =
			fplinux_rotate_plane_height(format, plane, height);
		uint32_t bytes = fplinux_rotate_row_bytes(format, plane, width);
		size_t size;

		guarded->image.plane[plane].stride = bytes + padding;
		size = (size_t)(bytes + padding) * rows;
		guarded->image.plane[plane].size = size;
		guarded->allocation[plane] = malloc(size + 2U * GUARD);
		if (!guarded->allocation[plane])
			return 0;
		memset(guarded->allocation[plane], GUARD_VALUE,
		       size + 2U * GUARD);
		guarded->image.plane[plane].data =
			guarded->allocation[plane] + GUARD;
	}
	return 1;
}

static void guarded_free(struct guarded_image *guarded)
{
	unsigned int plane;

	for (plane = 0; plane < 2U; ++plane)
		free(guarded->allocation[plane]);
}

static int guards_intact(const struct guarded_image *guarded)
{
	unsigned int plane;
	size_t index;

	for (plane = 0; plane < guarded->image.planes; ++plane)
		for (index = 0; index < GUARD; ++index)
			if (guarded->allocation[plane][index] != GUARD_VALUE ||
			    guarded->allocation[plane]
					       [GUARD +
						guarded->image.plane[plane].size +
						index] != GUARD_VALUE)
				return 0;
	return 1;
}

static unsigned int bytes_per_sample(enum fplinux_rotate_format format,
				     unsigned int plane)
{
	if (plane == 1U)
		return 2U;
	if (format == FPLINUX_ROTATE_RGB565)
		return 2U;
	if (format == FPLINUX_ROTATE_XRGB32)
		return 4U;
	return 1U;
}

static const uint8_t luma_mirror[] = {
	7,  6,	5,  4,	3,  2,	1,  0,	15, 14, 13, 12, 11, 10, 9,  8,
	23, 22, 21, 20, 19, 18, 17, 16, 31, 30, 29, 28, 27, 26, 25, 24,
	39, 38, 37, 36, 35, 34, 33, 32, 47, 46, 45, 44, 43, 42, 41, 40,
};
static const uint8_t luma_90[] = {
	40, 32, 24, 16, 8,  0,	41, 33, 25, 17, 9,  1,	42, 34, 26, 18,
	10, 2,	43, 35, 27, 19, 11, 3,	44, 36, 28, 20, 12, 4,	45, 37,
	29, 21, 13, 5,	46, 38, 30, 22, 14, 6,	47, 39, 31, 23, 15, 7,
};
static const uint8_t luma_180[] = {
	47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
	31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
	15, 14, 13, 12, 11, 10, 9,  8,	7,  6,	5,  4,	3,  2,	1,  0,
};
static const uint8_t luma_270[] = {
	7,  15, 23, 31, 39, 47, 6,  14, 22, 30, 38, 46, 5,  13, 21, 29,
	37, 45, 4,  12, 20, 28, 36, 44, 3,  11, 19, 27, 35, 43, 2,  10,
	18, 26, 34, 42, 1,  9,	17, 25, 33, 41, 0,  8,	16, 24, 32, 40,
};
static const uint8_t nv12_mirror[] = {
	3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8,
};
static const uint8_t nv12_90[] = {
	8, 4, 0, 9, 5, 1, 10, 6, 2, 11, 7, 3,
};
static const uint8_t nv12_180[] = {
	11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint8_t nv12_270[] = {
	3, 7, 11, 2, 6, 10, 1, 5, 9, 0, 4, 8,
};
static const uint8_t nv16_mirror[] = {
	3,  2,	1,  0,	7,  6,	5,  4,	11, 10, 9,  8,
	15, 14, 13, 12, 19, 18, 17, 16, 23, 22, 21, 20,
};
static const uint8_t nv16_90[] = {
	16, 8,	0, 16, 8,  0, 17, 9,  1, 17, 9,	 1,
	18, 10, 2, 18, 10, 2, 19, 11, 3, 19, 11, 3,
};
static const uint8_t nv16_180[] = {
	23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
	11, 10, 9,  8,	7,  6,	5,  4,	3,  2,	1,  0,
};
static const uint8_t nv16_270[] = {
	3, 11, 19, 3, 11, 19, 2, 10, 18, 2, 10, 18,
	1, 9,  17, 1, 9,  17, 0, 8,  16, 0, 8,	16,
};

static const uint8_t odd_nv16_y_mirror[] = {
	3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 19, 18, 17, 16,
};
static const uint8_t odd_nv16_y_90[] = {
	12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
};
static const uint8_t odd_nv16_y_180[] = {
	19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint8_t odd_nv16_y_270[] = {
	3, 7, 11, 15, 2, 6, 10, 14, 1, 5, 9, 13, 0, 4, 8, 12,
};
static const uint8_t odd_nv16_uv_mirror[] = {
	1, 0, 3, 2, 5, 4, 7, 6, 9, 8,
};
static const uint8_t odd_nv16_uv_90[] = {
	4, 0, 4, 0, 5, 1, 5, 1,
};
static const uint8_t odd_nv16_uv_180[] = {
	9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint8_t odd_nv16_uv_270[] = {
	1, 5, 1, 5, 0, 4, 0, 4,
};

static const uint8_t *literal_map(enum fplinux_rotate_format format,
				  unsigned int plane, unsigned int rotation,
				  int mirror, size_t *count,
				  uint32_t *source_width)
{
	const uint8_t *map;

	if (plane == 0U) {
		*count = sizeof(luma_mirror);
		*source_width = 8U;
		map = mirror	       ? luma_mirror :
		      rotation == 90U  ? luma_90 :
		      rotation == 180U ? luma_180 :
					 luma_270;
	} else if (format == FPLINUX_ROTATE_NV12) {
		*count = sizeof(nv12_mirror);
		*source_width = 4U;
		map = mirror	       ? nv12_mirror :
		      rotation == 90U  ? nv12_90 :
		      rotation == 180U ? nv12_180 :
					 nv12_270;
	} else {
		*count = sizeof(nv16_mirror);
		*source_width = 4U;
		map = mirror	       ? nv16_mirror :
		      rotation == 90U  ? nv16_90 :
		      rotation == 180U ? nv16_180 :
					 nv16_270;
	}
	return map;
}

struct expected_plane {
	const uint8_t *source_indexes;
	size_t index_count;
	uint32_t source_width;
	uint32_t source_left;
	uint32_t source_top;
	uint32_t destination_samples;
	uint32_t destination_rows;
	unsigned int sample_bytes;
};

static int
destination_padding_is_untouched(const struct fplinux_rotate_image *destination,
				 unsigned int plane, uint32_t rows,
				 uint32_t active_row_bytes)
{
	uint32_t row;

	for (row = 0; row < rows; ++row) {
		uint32_t byte;

		for (byte = active_row_bytes;
		     byte < destination->plane[plane].stride; ++byte)
			if (destination->plane[plane]
				    .data[(size_t)row * destination->plane[plane]
								.stride +
					  byte] != DESTINATION_VALUE)
				return 0;
	}
	return 1;
}

static int
expected_plane_matches(const struct fplinux_rotate_image *source,
		       const struct fplinux_rotate_image *destination,
		       unsigned int plane,
		       const struct expected_plane *expected)
{
	size_t index;

	if (expected->index_count !=
	    (size_t)expected->destination_samples * expected->destination_rows)
		return 0;
	for (index = 0; index < expected->index_count; ++index) {
		uint32_t destination_y = index / expected->destination_samples;
		uint32_t destination_x = index % expected->destination_samples;
		uint32_t source_x = expected->source_indexes[index] %
					    expected->source_width +
				    expected->source_left;
		uint32_t source_y = expected->source_indexes[index] /
					    expected->source_width +
				    expected->source_top;

		if (memcmp(destination->plane[plane].data +
				   (size_t)destination_y *
					   destination->plane[plane].stride +
				   destination_x * expected->sample_bytes,
			   source->plane[plane].data +
				   (size_t)source_y *
					   source->plane[plane].stride +
				   source_x * expected->sample_bytes,
			   expected->sample_bytes) != 0)
			return 0;
	}
	return destination_padding_is_untouched(
		destination, plane, expected->destination_rows,
		expected->destination_samples * expected->sample_bytes);
}

static int expected_matches(const struct fplinux_rotate_image *source,
			    const struct fplinux_rotate_image *destination,
			    const struct fplinux_rotate_transform *transform)
{
	unsigned int plane;

	for (plane = 0; plane < destination->planes; ++plane) {
		unsigned int sample_bytes =
			bytes_per_sample(source->format, plane);
		uint32_t width = destination->width;
		uint32_t height = fplinux_rotate_plane_height(
			destination->format, plane, destination->height);
		uint32_t samples = plane == 1U ? width / 2U : width;
		uint32_t source_width;
		uint32_t source_left = plane == 1U ? transform->left / 2U :
						     transform->left;
		uint32_t source_top =
			plane == 1U && source->format == FPLINUX_ROTATE_NV12 ?
				transform->top / 2U :
				transform->top;
		const uint8_t *map;
		size_t count;
		struct expected_plane expected;

		map = literal_map(source->format, plane, transform->rotation,
				  transform->hflip, &count, &source_width);
		expected = (struct expected_plane){
			.source_indexes = map,
			.index_count = count,
			.source_width = source_width,
			.source_left = source_left,
			.source_top = source_top,
			.destination_samples = samples,
			.destination_rows = height,
			.sample_bytes = sample_bytes,
		};
		if (!expected_plane_matches(source, destination, plane,
					    &expected))
			return 0;
	}
	return 1;
}

static int run_case(enum fplinux_rotate_format format, unsigned int rotation,
		    int mirror)
{
	struct fplinux_rotate_transform transform = {
		.left = 2,
		.top = 2,
		.width = 8,
		.height = 6,
		.rotation = rotation,
		.hflip = mirror,
	};
	struct guarded_image source;
	struct guarded_image destination;
	uint8_t *source_copy[2] = { NULL, NULL };
	uint32_t destination_width;
	uint32_t destination_height;
	unsigned int plane;
	int ok = 0;

	if (!fplinux_rotate_dimensions(&transform, &destination_width,
				       &destination_height) ||
	    !guarded_allocate(&source, format, 12, 10, 11) ||
	    !guarded_allocate(&destination, format, destination_width,
			      destination_height, 13))
		goto out;
	fplinux_rotate_fill_corpus(&source.image, 0x4bU);
	for (plane = 0; plane < source.image.planes; ++plane) {
		source_copy[plane] = malloc(source.image.plane[plane].size);
		if (!source_copy[plane])
			goto out;
		memcpy(source_copy[plane], source.image.plane[plane].data,
		       source.image.plane[plane].size);
	}
	for (plane = 0; plane < destination.image.planes; ++plane)
		memset(destination.image.plane[plane].data, DESTINATION_VALUE,
		       destination.image.plane[plane].size);
	if (!fplinux_rotate_cpu(&source.image, &destination.image,
				&transform) ||
	    !expected_matches(&source.image, &destination.image, &transform) ||
	    !guards_intact(&source) || !guards_intact(&destination))
		goto out;
	for (plane = 0; plane < source.image.planes; ++plane)
		if (memcmp(source_copy[plane], source.image.plane[plane].data,
			   source.image.plane[plane].size) != 0)
			goto out;
	ok = 1;
out:
	for (plane = 0; plane < 2U; ++plane)
		free(source_copy[plane]);
	guarded_free(&destination);
	guarded_free(&source);
	return ok;
}

static int
odd_nv16_expected_matches(const struct fplinux_rotate_image *source,
			  const struct fplinux_rotate_image *destination,
			  const struct fplinux_rotate_transform *transform,
			  const uint8_t *y_indexes, size_t y_count,
			  const uint8_t *uv_indexes, size_t uv_count)
{
	const uint8_t *indexes[2] = { y_indexes, uv_indexes };
	const size_t counts[2] = { y_count, uv_count };
	unsigned int plane;

	for (plane = 0; plane < 2U; ++plane) {
		unsigned int sample_bytes = plane == 0U ? 1U : 2U;
		uint32_t crop_samples = plane == 0U ? transform->width :
						      transform->width / 2U;
		uint32_t output_samples = plane == 0U ? destination->width :
							destination->width / 2U;
		uint32_t output_rows = destination->height;
		uint32_t source_left = plane == 0U ? transform->left :
						     transform->left / 2U;
		struct expected_plane expected = {
			.source_indexes = indexes[plane],
			.index_count = counts[plane],
			.source_width = crop_samples,
			.source_left = source_left,
			.source_top = transform->top,
			.destination_samples = output_samples,
			.destination_rows = output_rows,
			.sample_bytes = sample_bytes,
		};

		if (!expected_plane_matches(source, destination, plane,
					    &expected))
			return 0;
	}
	return 1;
}

static int run_odd_nv16_case(unsigned int rotation, int mirror)
{
	struct fplinux_rotate_transform transform = {
		.left = 2,
		.top = 1,
		.width = 4,
		.height = rotation == 90U || rotation == 270U ? 4U : 5U,
		.rotation = rotation,
		.hflip = mirror,
	};
	const uint8_t *y_indexes = mirror	    ? odd_nv16_y_mirror :
				   rotation == 90U  ? odd_nv16_y_90 :
				   rotation == 180U ? odd_nv16_y_180 :
						      odd_nv16_y_270;
	const uint8_t *uv_indexes = mirror	     ? odd_nv16_uv_mirror :
				    rotation == 90U  ? odd_nv16_uv_90 :
				    rotation == 180U ? odd_nv16_uv_180 :
						       odd_nv16_uv_270;
	size_t y_count = mirror		  ? sizeof(odd_nv16_y_mirror) :
			 rotation == 90U  ? sizeof(odd_nv16_y_90) :
			 rotation == 180U ? sizeof(odd_nv16_y_180) :
					    sizeof(odd_nv16_y_270);
	size_t uv_count = mirror	   ? sizeof(odd_nv16_uv_mirror) :
			  rotation == 90U  ? sizeof(odd_nv16_uv_90) :
			  rotation == 180U ? sizeof(odd_nv16_uv_180) :
					     sizeof(odd_nv16_uv_270);
	struct guarded_image source;
	struct guarded_image destination;
	uint8_t *source_copy[2] = { NULL, NULL };
	uint32_t destination_width;
	uint32_t destination_height;
	unsigned int plane;
	int ok = 0;

	if (!fplinux_rotate_dimensions(&transform, &destination_width,
				       &destination_height) ||
	    !guarded_allocate(&source, FPLINUX_ROTATE_NV16, 8, 7, 3) ||
	    !guarded_allocate(&destination, FPLINUX_ROTATE_NV16,
			      destination_width, destination_height, 5))
		goto out;
	for (plane = 0; plane < source.image.planes; ++plane) {
		uint32_t y;

		memset(source.image.plane[plane].data, 0xee,
		       source.image.plane[plane].size);
		for (y = 0; y < source.image.height; ++y) {
			uint32_t x;

			for (x = 0; x < source.image.width; ++x) {
				if (plane == 0U)
					source.image.plane[plane].data
						[(size_t)y *
							 source.image
								 .plane[plane]
								 .stride +
						 x] =
						(uint8_t)(1U + y * 8U + x);
				else
					source.image.plane[plane].data
						[(size_t)y *
							 source.image
								 .plane[plane]
								 .stride +
						 x] =
						(uint8_t)(0x61U + y * 8U + x);
			}
		}
		source_copy[plane] = malloc(source.image.plane[plane].size);
		if (!source_copy[plane])
			goto out;
		memcpy(source_copy[plane], source.image.plane[plane].data,
		       source.image.plane[plane].size);
	}
	for (plane = 0; plane < destination.image.planes; ++plane)
		memset(destination.image.plane[plane].data, DESTINATION_VALUE,
		       destination.image.plane[plane].size);
	if (!fplinux_rotate_cpu(&source.image, &destination.image,
				&transform) ||
	    !odd_nv16_expected_matches(&source.image, &destination.image,
				       &transform, y_indexes, y_count,
				       uv_indexes, uv_count) ||
	    !guards_intact(&source) || !guards_intact(&destination))
		goto out;
	for (plane = 0; plane < source.image.planes; ++plane)
		if (memcmp(source_copy[plane], source.image.plane[plane].data,
			   source.image.plane[plane].size) != 0)
			goto out;
	ok = 1;
out:
	for (plane = 0; plane < 2U; ++plane)
		free(source_copy[plane]);
	guarded_free(&destination);
	guarded_free(&source);
	return ok;
}

static int preview_conversion_is_stable(void)
{
	uint8_t pixels[] = {
		0xa5, 0xff, 0x00, 0x00, 0x5a, 0x00,
		0xff, 0x00, 0xc3, 0x00, 0x00, 0xff,
	};
	struct fplinux_rotate_image image = {
		.format = FPLINUX_ROTATE_XRGB32,
		.width = 3,
		.height = 1,
		.planes = 1,
		.plane = { { pixels, sizeof(pixels), 12 } },
	};
	uint16_t output[3] = { 0, 0, 0 };

	return fplinux_rotate_to_rgb565(&image, output, 3) &&
	       output[0] == 0xf800U && output[1] == 0x07e0U &&
	       output[2] == 0x001fU;
}

int main(void)
{
	static const enum fplinux_rotate_format formats[] = {
		FPLINUX_ROTATE_RGB565, FPLINUX_ROTATE_XRGB32,
		FPLINUX_ROTATE_GREY,   FPLINUX_ROTATE_NV12,
		FPLINUX_ROTATE_NV16,
	};
	static const struct transform_case regular_cases[] = {
		{ 0U, 1 },
		{ 90U, 0 },
		{ 180U, 0 },
		{ 270U, 0 },
	};
	unsigned int format;
	unsigned int transform;

	for (format = 0; format < sizeof(formats) / sizeof(formats[0]);
	     ++format)
		for (transform = 0;
		     transform <
		     sizeof(regular_cases) / sizeof(regular_cases[0]);
		     ++transform)
			if (!run_case(formats[format],
				      regular_cases[transform].rotation,
				      regular_cases[transform].mirror))
				return EXIT_FAILURE;
	for (transform = 0;
	     transform < sizeof(regular_cases) / sizeof(regular_cases[0]);
	     ++transform)
		if (!run_odd_nv16_case(regular_cases[transform].rotation,
				       regular_cases[transform].mirror))
			return EXIT_FAILURE;
	return preview_conversion_is_stable() ? EXIT_SUCCESS : EXIT_FAILURE;
}
