/* SPDX-License-Identifier: 0BSD */
#include "resize-image.h"

#include <stdlib.h>

int mre_gif_resize(struct mre_gif_image *image, int width, int height)
{
	if (!image || !image->pixels || !image->opaque || image->width <= 0 ||
	    image->height <= 0 || width <= 0 || height <= 0 || width > 512 ||
	    height > 512)
		return -1;
	if (width == image->width && height == image->height)
		return 0;
	size_t count = (size_t)width * (size_t)height;
	uint16_t *pixels = malloc(count * sizeof(*pixels));
	uint8_t *opaque = malloc(count);
	if (!pixels || !opaque) {
		free(pixels);
		free(opaque);
		return -1;
	}
	for (int y = 0; y < height; ++y) {
		size_t source_y =
			(size_t)y * (size_t)image->height / (size_t)height;
		for (int x = 0; x < width; ++x) {
			size_t source_x = (size_t)x * (size_t)image->width /
					  (size_t)width;
			size_t source =
				source_y * (size_t)image->width + source_x;
			size_t destination =
				(size_t)y * (size_t)width + (size_t)x;
			pixels[destination] = image->pixels[source];
			opaque[destination] = image->opaque[source];
		}
	}
	free(image->pixels);
	free(image->opaque);
	image->pixels = pixels;
	image->opaque = opaque;
	image->width = width;
	image->height = height;
	return 0;
}
