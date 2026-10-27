/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_IMAGE_DECODE_H
#define FPLINUX_MRE_IMAGE_DECODE_H

#include <stddef.h>
#include <stdint.h>

struct mre_image_limits {
	size_t max_input_bytes;
	unsigned int max_width;
	unsigned int max_height;
	size_t max_pixels;
	size_t max_working_bytes;
};

struct mre_image {
	int width;
	int height;
	uint16_t *pixels;
	uint8_t *alpha;
	uint16_t transparent_color;
	int has_transparency;
	int has_partial_alpha;
};

/*
 * Decode memory into native RGB565 (high-bit truncation) and alpha (0..255).
 * Fully transparent pixels use a color absent from all nonzero-alpha pixels.
 * Partial alpha remains in the separate plane; a caller must blend it.
 * Limits bound input, dimensions, pixel count and decoder/output allocations.
 * The working bound includes conservative libjpeg initialization overhead.
 * Output must be empty. Returns 0 on success, -1 with empty output on failure.
 * Both arrays belong to the caller and are released by mre_image_free().
 * Error may be NULL. The ARM libjpeg-turbo backend must use WITH_SIMD=OFF;
 * its runtime SIMD detection otherwise opens /proc/cpuinfo before decoding.
 * With that backend these functions never open files or use devices.
 */
int mre_png_decode(const void *data, size_t size,
		   const struct mre_image_limits *limits,
		   struct mre_image *image, char *error, size_t error_size);

/* JPEG supports the proven service input: 8-bit, single-scan sequential
 * grayscale/RGB/YCbCr. Progressive, lossless, arithmetic and CMYK are rejected.
 */
int mre_jpeg_decode(const void *data, size_t size,
		    const struct mre_image_limits *limits,
		    struct mre_image *image, char *error, size_t error_size);
void mre_image_free(struct mre_image *image);

#endif
