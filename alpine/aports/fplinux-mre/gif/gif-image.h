/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_GIF_IMAGE_H
#define FPLINUX_MRE_GIF_IMAGE_H

#include <stddef.h>
#include <stdint.h>

struct mre_gif_image {
	int width;
	int height;
	uint16_t *pixels;
	uint8_t *opaque;
	uint8_t transparent_index;
	uint16_t transparent_color;
	int has_transparency;
};

/*
 * Decode the first frame into native uint16_t RGB565 pixels and opacity bytes
 * (0 transparent, 1 opaque). Returns 0 on success, -1 on failure. The output
 * must be empty before decoding; on failure it is empty. The caller owns both
 * arrays on success and releases them with mre_gif_free(). Error may be NULL.
 */
int mre_gif_decode(const void *data, size_t size, struct mre_gif_image *image,
		   char *error, size_t error_size);
void mre_gif_free(struct mre_gif_image *image);

#endif
