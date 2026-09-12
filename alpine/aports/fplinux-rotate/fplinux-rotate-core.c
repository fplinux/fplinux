/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-rotate.h"

#include <limits.h>
#include <string.h>

unsigned int fplinux_rotate_plane_count(enum fplinux_rotate_format format)
{
	return format == FPLINUX_ROTATE_NV12 || format == FPLINUX_ROTATE_NV16 ?
		       2U :
		       1U;
}

uint32_t fplinux_rotate_row_bytes(enum fplinux_rotate_format format,
				  unsigned int plane, uint32_t width)
{
	if (plane > 1U ||
	    (plane == 1U && fplinux_rotate_plane_count(format) != 2U))
		return 0;
	if (plane == 1U)
		return width;
	switch (format) {
	case FPLINUX_ROTATE_RGB565:
		return width <= UINT32_MAX / 2U ? width * 2U : 0;
	case FPLINUX_ROTATE_XRGB32:
		return width <= UINT32_MAX / 4U ? width * 4U : 0;
	case FPLINUX_ROTATE_GREY:
	case FPLINUX_ROTATE_NV12:
	case FPLINUX_ROTATE_NV16:
		return width;
	}
	return 0;
}

uint32_t fplinux_rotate_plane_height(enum fplinux_rotate_format format,
				     unsigned int plane, uint32_t height)
{
	if (plane == 0U)
		return height;
	if (plane != 1U)
		return 0;
	if (format == FPLINUX_ROTATE_NV12)
		return height / 2U + height % 2U;
	if (format == FPLINUX_ROTATE_NV16)
		return height;
	return 0;
}

bool fplinux_rotate_dimensions(const struct fplinux_rotate_transform *transform,
			       uint32_t *width, uint32_t *height)
{
	if (!transform || !width || !height || transform->width == 0U ||
	    transform->height == 0U ||
	    (transform->rotation != 0U && transform->rotation != 90U &&
	     transform->rotation != 180U && transform->rotation != 270U))
		return false;
	if (transform->rotation == 90U || transform->rotation == 270U) {
		*width = transform->height;
		*height = transform->width;
	} else {
		*width = transform->width;
		*height = transform->height;
	}
	return true;
}

static bool plane_valid(const struct fplinux_rotate_image *image,
			unsigned int plane)
{
	const struct fplinux_rotate_plane *selected;
	uint32_t rows;
	uint32_t bytes;

	if (plane >= 2U)
		return false;
	selected = plane == 0U ? &image->plane[0] : &image->plane[1];
	rows = fplinux_rotate_plane_height(image->format, plane, image->height);
	bytes = fplinux_rotate_row_bytes(image->format, plane, image->width);

	return selected->data && bytes && rows && selected->stride >= bytes &&
	       (size_t)selected->stride <= SIZE_MAX / rows &&
	       selected->size >= (size_t)selected->stride * rows;
}

bool fplinux_rotate_validate(const struct fplinux_rotate_image *source,
			     const struct fplinux_rotate_image *destination,
			     const struct fplinux_rotate_transform *transform)
{
	uint32_t output_width;
	uint32_t output_height;
	unsigned int plane;

	if (!source || !destination || !transform ||
	    source->format != destination->format ||
	    source->planes != fplinux_rotate_plane_count(source->format) ||
	    destination->planes != source->planes ||
	    !fplinux_rotate_dimensions(transform, &output_width,
				       &output_height) ||
	    destination->width != output_width ||
	    destination->height != output_height ||
	    transform->left > source->width ||
	    transform->top > source->height ||
	    transform->width > source->width - transform->left ||
	    transform->height > source->height - transform->top)
		return false;
	if (source->format == FPLINUX_ROTATE_NV12 &&
	    ((source->width | source->height | transform->left |
	      transform->top | transform->width | transform->height |
	      output_width) &
	     1U))
		return false;
	if (source->format == FPLINUX_ROTATE_NV16 &&
	    ((source->width | transform->left | transform->width |
	      output_width) &
	     1U))
		return false;
	for (plane = 0; plane < source->planes; ++plane)
		if (!plane_valid(source, plane) ||
		    !plane_valid(destination, plane))
			return false;
	return true;
}

static void map_pixel(const struct fplinux_rotate_transform *transform,
		      uint32_t dx, uint32_t dy, uint32_t *sx, uint32_t *sy)
{
	uint32_t output_width = transform->rotation == 90U ||
						transform->rotation == 270U ?
					transform->height :
					transform->width;
	uint32_t output_height = transform->rotation == 90U ||
						 transform->rotation == 270U ?
					 transform->width :
					 transform->height;
	uint32_t x = transform->hflip ? output_width - 1U - dx : dx;
	uint32_t y = transform->vflip ? output_height - 1U - dy : dy;

	switch (transform->rotation) {
	case 0:
		*sx = x;
		*sy = y;
		break;
	case 90:
		*sx = y;
		*sy = transform->height - 1U - x;
		break;
	case 180:
		*sx = transform->width - 1U - x;
		*sy = transform->height - 1U - y;
		break;
	default:
		*sx = transform->width - 1U - y;
		*sy = x;
		break;
	}
	*sx += transform->left;
	*sy += transform->top;
}

static void rotate_grid(const uint8_t *source, uint32_t source_stride,
			uint8_t *destination, uint32_t destination_stride,
			uint32_t source_left, uint32_t source_top,
			uint32_t source_width, uint32_t source_height,
			uint32_t output_width, uint32_t output_height,
			unsigned int bytes_per_sample,
			const struct fplinux_rotate_transform *transform)
{
	uint32_t dx;
	uint32_t dy;
	struct fplinux_rotate_transform grid = *transform;

	grid.left = source_left;
	grid.top = source_top;
	grid.width = source_width;
	grid.height = source_height;
	for (dy = 0; dy < output_height; ++dy) {
		for (dx = 0; dx < output_width; ++dx) {
			uint32_t sx;
			uint32_t sy;

			map_pixel(&grid, dx, dy, &sx, &sy);
			memcpy(destination + (size_t)dy * destination_stride +
				       (size_t)dx * bytes_per_sample,
			       source + (size_t)sy * source_stride +
				       (size_t)sx * bytes_per_sample,
			       bytes_per_sample);
		}
	}
}

bool fplinux_rotate_cpu(const struct fplinux_rotate_image *source,
			const struct fplinux_rotate_image *destination,
			const struct fplinux_rotate_transform *transform)
{
	uint32_t output_width;
	uint32_t output_height;
	unsigned int bytes_per_pixel;

	if (!fplinux_rotate_validate(source, destination, transform) ||
	    !fplinux_rotate_dimensions(transform, &output_width,
				       &output_height))
		return false;
	bytes_per_pixel = source->format == FPLINUX_ROTATE_RGB565 ? 2U :
			  source->format == FPLINUX_ROTATE_XRGB32 ? 4U :
								    1U;
	rotate_grid(source->plane[0].data, source->plane[0].stride,
		    destination->plane[0].data, destination->plane[0].stride,
		    transform->left, transform->top, transform->width,
		    transform->height, output_width, output_height,
		    bytes_per_pixel, transform);
	if (source->format == FPLINUX_ROTATE_NV16 &&
	    (transform->rotation == 90U || transform->rotation == 270U)) {
		uint32_t dx;
		uint32_t dy;

		/* ROTA's 4:2:2 quarter-turn mode keeps horizontal subsampling by
		 * selecting every other source row and duplicating chroma vertically. */
		for (dy = 0; dy < output_height; ++dy) {
			for (dx = 0; dx < output_width / 2U; ++dx) {
				uint32_t sx;
				uint32_t sy;

				if (transform->rotation == 90U) {
					sx = dy / 2U;
					sy = transform->height - 2U - dx * 2U;
				} else {
					sx = transform->width / 2U - 1U -
					     dy / 2U;
					sy = dx * 2U;
				}
				sx += transform->left / 2U;
				sy += transform->top;
				memcpy(destination->plane[1].data +
					       (size_t)dy *
						       destination->plane[1]
							       .stride +
					       dx * 2U,
				       source->plane[1].data +
					       (size_t)sy *
						       source->plane[1].stride +
					       sx * 2U,
				       2U);
			}
		}
	} else if (source->planes == 2U) {
		uint32_t chroma_height = source->format == FPLINUX_ROTATE_NV12 ?
						 transform->height / 2U :
						 transform->height;
		uint32_t output_chroma_height =
			source->format == FPLINUX_ROTATE_NV12 ?
				output_height / 2U :
				output_height;

		rotate_grid(source->plane[1].data, source->plane[1].stride,
			    destination->plane[1].data,
			    destination->plane[1].stride, transform->left / 2U,
			    source->format == FPLINUX_ROTATE_NV12 ?
				    transform->top / 2U :
				    transform->top,
			    transform->width / 2U, chroma_height,
			    output_width / 2U, output_chroma_height, 2U,
			    transform);
	}
	return true;
}

static uint16_t pack_rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
	return (uint16_t)(((uint16_t)(red & 0xf8U) << 8) |
			  ((uint16_t)(green & 0xfcU) << 3) | (blue >> 3));
}

static uint8_t clamp_component(int value)
{
	return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

bool fplinux_rotate_to_rgb565(const struct fplinux_rotate_image *source,
			      uint16_t *destination, uint32_t stride_pixels)
{
	uint32_t x;
	uint32_t y;

	if (!source || !destination || stride_pixels < source->width ||
	    source->planes != fplinux_rotate_plane_count(source->format) ||
	    !plane_valid(source, 0) ||
	    (source->planes == 2U && !plane_valid(source, 1)))
		return false;
	for (y = 0; y < source->height; ++y) {
		for (x = 0; x < source->width; ++x) {
			const uint8_t *pixel;
			uint16_t value;

			if (source->format == FPLINUX_ROTATE_RGB565) {
				memcpy(&value,
				       source->plane[0].data +
					       (size_t)y *
						       source->plane[0].stride +
					       x * 2U,
				       sizeof(value));
			} else if (source->format == FPLINUX_ROTATE_XRGB32) {
				pixel = source->plane[0].data +
					(size_t)y * source->plane[0].stride +
					x * 4U;
				value = pack_rgb565(pixel[1], pixel[2],
						    pixel[3]);
			} else if (source->format == FPLINUX_ROTATE_GREY) {
				uint8_t grey =
					source->plane[0]
						.data[(size_t)y *
							      source->plane[0]
								      .stride +
						      x];
				value = pack_rgb565(grey, grey, grey);
			} else {
				int yy = source->plane[0]
						 .data[(size_t)y *
							       source->plane[0]
								       .stride +
						       x];
				const uint8_t *uv =
					source->plane[1].data +
					(source->format == FPLINUX_ROTATE_NV12 ?
						 y / 2U :
						 y) *
						source->plane[1].stride +
					(x & ~1U);
				int u = (int)uv[0] - 128;
				int v = (int)uv[1] - 128;
				int c = yy - 16;

				value = pack_rgb565(
					clamp_component(
						(298 * c + 409 * v + 128) >> 8),
					clamp_component((298 * c - 100 * u -
							 208 * v + 128) >>
							8),
					clamp_component(
						(298 * c + 516 * u + 128) >>
						8));
			}
			destination[(size_t)y * stride_pixels + x] = value;
		}
	}
	return true;
}

void fplinux_rotate_fill_corpus(struct fplinux_rotate_image *image,
				uint32_t seed)
{
	unsigned int plane;

	if (!image)
		return;
	for (plane = 0; plane < image->planes; ++plane) {
		uint32_t rows = fplinux_rotate_plane_height(
			image->format, plane, image->height);
		uint32_t row;
		uint32_t column;

		for (row = 0; row < rows; ++row)
			for (column = 0; column < image->plane[plane].stride;
			     ++column)
				image->plane[plane]
					.data[(size_t)row * image->plane[plane]
								    .stride +
					      column] =
					(uint8_t)(seed + plane * 97U +
						  row * 29U + column * 53U +
						  row * column * 7U);
	}
}
