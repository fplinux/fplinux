/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_ROTATE_H
#define FPLINUX_ROTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fplinux_rotate_format {
	FPLINUX_ROTATE_RGB565,
	FPLINUX_ROTATE_XRGB32,
	FPLINUX_ROTATE_GREY,
	FPLINUX_ROTATE_NV12,
	FPLINUX_ROTATE_NV16,
};

struct fplinux_rotate_plane {
	uint8_t *data;
	size_t size;
	uint32_t stride;
};

struct fplinux_rotate_image {
	enum fplinux_rotate_format format;
	uint32_t width;
	uint32_t height;
	unsigned int planes;
	struct fplinux_rotate_plane plane[2];
};

struct fplinux_rotate_transform {
	uint32_t left;
	uint32_t top;
	uint32_t width;
	uint32_t height;
	unsigned int rotation;
	bool hflip;
	bool vflip;
};

unsigned int fplinux_rotate_plane_count(enum fplinux_rotate_format format);
uint32_t fplinux_rotate_row_bytes(enum fplinux_rotate_format format,
				  unsigned int plane, uint32_t width);
uint32_t fplinux_rotate_plane_height(enum fplinux_rotate_format format,
				     unsigned int plane, uint32_t height);
bool fplinux_rotate_dimensions(const struct fplinux_rotate_transform *transform,
			       uint32_t *width, uint32_t *height);
bool fplinux_rotate_validate(const struct fplinux_rotate_image *source,
			     const struct fplinux_rotate_image *destination,
			     const struct fplinux_rotate_transform *transform);
bool fplinux_rotate_cpu(const struct fplinux_rotate_image *source,
			const struct fplinux_rotate_image *destination,
			const struct fplinux_rotate_transform *transform);
bool fplinux_rotate_to_rgb565(const struct fplinux_rotate_image *source,
			      uint16_t *destination, uint32_t stride_pixels);
void fplinux_rotate_fill_corpus(struct fplinux_rotate_image *image,
				uint32_t seed);

#endif
