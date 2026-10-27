/* SPDX-License-Identifier: 0BSD */
/* fplinux-check: package-embedded
 * The decoder header is checksum-pinned and prepared by this aport. */
#include "gif-image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIF_MAX_WIDTH 2048
#define GIF_IMPLEMENTATION
#include "gif.h"

#define MRE_GIF_MAX_WIDTH 2048
#define MRE_GIF_MAX_HEIGHT 512
#define MRE_GIF_MAX_PIXELS 262144U
#define MRE_GIF_MAX_INPUT_BYTES (4U * 1024U * 1024U)

struct image_renderer {
	struct mre_gif_image *image;
	GIF_Context *decoder;
	size_t rendered_pixels;
	int initialized;
	int invalid;
};

static uint16_t rgb565(const uint8_t *rgb)
{
	return ((uint16_t)(rgb[0] >> 3) << 11) |
	       ((uint16_t)(rgb[1] >> 2) << 5) | (rgb[2] >> 3);
}

static void set_error(char *error, size_t size, const char *message)
{
	if (error && size)
		snprintf(error, size, "%s", message);
}

static void render_indexed(void *user, int x, int y, int width, int height,
			   const gif_u8 *indices, int stride,
			   const gif_u8 *palette, int palette_colors,
			   int transparent_index, int has_transparency)
{
	struct image_renderer *renderer = user;
	struct mre_gif_image *image = renderer->image;
	size_t pixel_count = (size_t)image->width * image->height;
	int row, column;

	if (renderer->invalid)
		return;
	if (!indices || !palette || palette_colors < 1 ||
	    palette_colors > 256 || x < 0 || y < 0 || width <= 0 ||
	    height <= 0 || x > image->width || y > image->height ||
	    width > image->width - x || height > image->height - y ||
	    stride < width) {
		renderer->invalid = 1;
		return;
	}
	if (!renderer->initialized) {
		uint16_t background = rgb565(renderer->decoder->bg_rgb);
		size_t i;

		image->has_transparency = has_transparency != 0;
		image->transparent_index = (uint8_t)transparent_index;
		for (i = 0; i < pixel_count; i++)
			image->pixels[i] = background;
		memset(image->opaque, has_transparency ? 0 : 1, pixel_count);
		renderer->initialized = 1;
	}
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			uint8_t index = indices[(size_t)row * stride + column];
			size_t pixel =
				(size_t)(y + row) * image->width + x + column;

			if (has_transparency && index == transparent_index) {
				image->opaque[pixel] = 0;
			} else if (index < palette_colors) {
				image->pixels[pixel] =
					rgb565(palette + (size_t)index * 3);
				image->opaque[pixel] = 1;
			} else {
				renderer->invalid = 1;
				return;
			}
		}
	}
	renderer->rendered_pixels += (size_t)width * height;
}

static uint16_t unused_transparent_color(const GIF_Context *decoder)
{
	uint16_t candidate = 0xf81f;
	unsigned int i;

	/* At most 256 palette entries occupy the 65536 RGB565 values. */
	for (;;) {
		for (i = 0; i < decoder->apal_n; i++) {
			if (i != decoder->transparent_index &&
			    rgb565(decoder->apal + i * 3) == candidate)
				break;
		}
		if (i == decoder->apal_n)
			return candidate;
		candidate++;
	}
}

void mre_gif_free(struct mre_gif_image *image)
{
	if (!image)
		return;
	free(image->pixels);
	free(image->opaque);
	memset(image, 0, sizeof(*image));
}

int mre_gif_decode(const void *data, size_t size, struct mre_gif_image *image,
		   char *error, size_t error_size)
{
	GIF_Context decoder;
	struct image_renderer renderer = { .image = image,
					   .decoder = &decoder };
	GIF_Renderer callbacks = { .user = &renderer,
				   .blit_indexed = render_indexed };
	uint8_t *scratch = NULL;
	size_t pixel_count, i;
	int result, delay_ms;

	set_error(error, error_size, "");
	if (!image) {
		set_error(error, error_size, "Missing GIF image output");
		return -1;
	}
	memset(image, 0, sizeof(*image));
	if (!data || size < 13 || size > MRE_GIF_MAX_INPUT_BYTES) {
		set_error(error, error_size, "Invalid GIF input size");
		return -1;
	}
	scratch = malloc(GIF_SCRATCH_BUFFER_REQUIRED_SIZE);
	if (!scratch) {
		set_error(error, error_size,
			  "Cannot allocate GIF scratch buffer");
		return -1;
	}
	result = gif_init(&decoder, data, size, scratch,
			  GIF_SCRATCH_BUFFER_REQUIRED_SIZE);
	if (result != GIF_SUCCESS) {
		set_error(error, error_size, gif_get_error_string(result));
		goto fail;
	}
	if (decoder.canvas_w > MRE_GIF_MAX_WIDTH ||
	    decoder.canvas_h > MRE_GIF_MAX_HEIGHT) {
		set_error(error, error_size,
			  "GIF dimensions exceed bounded 1024x512 canvas");
		goto fail;
	}
	image->width = (int)decoder.canvas_w;
	image->height = (int)decoder.canvas_h;
	pixel_count = (size_t)image->width * image->height;
	if (!pixel_count || pixel_count > MRE_GIF_MAX_PIXELS) {
		set_error(error, error_size, "Invalid GIF pixel count");
		goto fail;
	}
	image->pixels = malloc(pixel_count * sizeof(*image->pixels));
	image->opaque = malloc(pixel_count);
	if (!image->pixels || !image->opaque) {
		set_error(error, error_size,
			  "Cannot allocate GIF image buffers");
		goto fail;
	}
	result = gif_next_frame_render(&decoder, &callbacks, &delay_ms);
	if (result != GIF_SUCCESS) {
		if (error && error_size)
			snprintf(error, error_size,
				 "%s (rendered=%zu expected=%zu)",
				 gif_get_error_string(result),
				 renderer.rendered_pixels,
				 (size_t)decoder.fw * decoder.fh);
		goto fail;
	}
	if (renderer.invalid || !renderer.initialized ||
	    renderer.rendered_pixels != (size_t)decoder.fw * decoder.fh) {
		set_error(error, error_size,
			  "Incomplete or invalid GIF first frame");
		goto fail;
	}
	image->transparent_color = image->has_transparency ?
					   unused_transparent_color(&decoder) :
					   0xffff;
	for (i = 0; i < pixel_count; i++) {
		if (!image->opaque[i])
			image->pixels[i] = image->transparent_color;
	}
	gif_close(&decoder);
	free(scratch);
	return 0;

fail:
	free(scratch);
	mre_gif_free(image);
	return -1;
}
