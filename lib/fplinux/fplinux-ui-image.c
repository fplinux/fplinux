/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-ui-screen.h"
#include "fplinux-ui-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <lvgl/src/libs/lodepng/lodepng.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Image sources the runtime decodes: file size and decoded pixels. */
#define IMAGE_FILE_MAX_BYTES (1024 * 1024)
#define IMAGE_MAX_PIXELS (1024U * 1024U)
/* A file with this ending is decoded as an alpha mask. */
#define SYMBOLIC_SUFFIX "-symbolic.png"

static const uint8_t png_signature[8] = { 0x89, 'P',  'N',  'G',
					  '\r', '\n', 0x1a, '\n' };

static uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static bool read_all(int fd, uint8_t *data, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t count = read(fd, data + done, size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EINVAL;
			return false;
		}
		done += (size_t)count;
	}
	return true;
}

/* Reads a PNG file whole into a descriptor of LVGL's PNG decoder. */
static uint8_t *read_png(const char *path, lv_image_dsc_t *image)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat status;
	uint8_t *data = NULL;
	int error;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &status) < 0)
		goto fail;
	if (!S_ISREG(status.st_mode) || status.st_size > IMAGE_FILE_MAX_BYTES) {
		errno = S_ISREG(status.st_mode) ? EFBIG : EINVAL;
		goto fail;
	}
	data = malloc(status.st_size ? (size_t)status.st_size : 1);
	if (!data || !read_all(fd, data, (size_t)status.st_size))
		goto fail;
	/* The signature, then IHDR: 13-byte length, type, width and height. */
	if (status.st_size < 24 ||
	    memcmp(data, png_signature, sizeof(png_signature)) ||
	    read_be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) ||
	    read_be32(data + 16) > UINT16_MAX ||
	    read_be32(data + 20) > UINT16_MAX) {
		errno = EINVAL;
		goto fail;
	}
	close(fd);
	*image = (lv_image_dsc_t){
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = read_be32(data + 16),
			.h = read_be32(data + 20),
		},
		.data_size = (uint32_t)status.st_size,
		.data = data,
	};
	return data;
fail:
	error = errno;
	free(data);
	close(fd);
	errno = error;
	return NULL;
}

/*
 * Draws a native image once, at its own size, into a new ARGB8888 buffer.
 * An alpha-only source draws its mask in black.
 */
static lv_draw_buf_t *draw_image(const void *src,
				 const lv_image_header_t *header)
{
	const lv_area_t area = { 0, 0, (int32_t)header->w - 1,
				 (int32_t)header->h - 1 };
	lv_draw_buf_t *buffer = lv_draw_buf_create(
		header->w, header->h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
	lv_draw_image_dsc_t image;
	lv_layer_t layer;
	lv_obj_t *canvas;

	if (!buffer)
		return NULL;
	canvas = lv_canvas_create(NULL);
	if (!canvas) {
		lv_draw_buf_destroy(buffer);
		return NULL;
	}
	lv_draw_buf_clear(buffer, NULL);
	lv_canvas_set_draw_buf(canvas, buffer);
	lv_canvas_init_layer(canvas, &layer);
	lv_draw_image_dsc_init(&image);
	image.src = src;
	image.scale_x = LV_SCALE_NONE;
	image.scale_y = LV_SCALE_NONE;
	image.pivot.x = 0;
	image.pivot.y = 0;
	lv_draw_image(&layer, &image, &area);
	lv_canvas_finish_layer(canvas, &layer);
	lv_obj_delete(canvas);
	return buffer;
}

static unsigned int decode_preview_png(const lv_image_dsc_t *source,
				       unsigned char **decoded,
				       unsigned int *width,
				       unsigned int *height)
{
	LodePNGState state;
	unsigned int error;

	lodepng_state_init(&state);
	state.info_raw.colortype = LCT_RGBA;
	state.info_raw.bitdepth = 8;
	/* Four bytes per pixel and one filter byte per row in each pass. */
	state.decoder.zlibsettings.max_output_size =
		4U * source->header.w * source->header.h +
		7U * source->header.h;
#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
	state.decoder.read_text_chunks = 0;
	state.decoder.max_icc_size = 64U * 1024U;
#endif
	error = lodepng_decode(decoded, width, height, &state, source->data,
			       source->data_size);
	lodepng_state_cleanup(&state);
	return error;
}

static lv_draw_buf_t *decode_image(const lv_image_dsc_t *source,
				   const lv_image_header_t *header,
				   bool preview)
{
	bool png =
		source->header.cf == LV_COLOR_FORMAT_RAW ||
		source->header.cf == LV_COLOR_FORMAT_RAW_ALPHA ||
		(source->data_size >= sizeof(png_signature) &&
		 !memcmp(source->data, png_signature, sizeof(png_signature)));
	unsigned char *decoded = NULL;
	lv_draw_buf_t *buffer;
	unsigned int width, height;
	unsigned int error;

	if (!png) {
		buffer = draw_image(source, header);
		if (!buffer)
			errno = ENOMEM;
		return buffer;
	}
	error = preview ?
			decode_preview_png(source, &decoded, &width, &height) :
			lodepng_decode32(&decoded, &width, &height,
					 source->data, source->data_size);
	buffer = (lv_draw_buf_t *)decoded;
	if (error) {
		if (buffer)
			lv_draw_buf_destroy(buffer);
		/* LodePNG reports allocation failure with error 83. */
		errno = error == 83 ? ENOMEM : EINVAL;
		return NULL;
	}
	/* The PNG decoder returns an owned draw buffer in RGBA byte order. */
	for (uint32_t y = 0; y < buffer->header.h; ++y) {
		lv_color32_t *row = (lv_color32_t *)(buffer->data +
						     y * buffer->header.stride);

		for (uint32_t x = 0; x < buffer->header.w; ++x) {
			uint8_t red = row[x].blue;

			row[x].blue = row[x].red;
			row[x].red = red;
		}
	}
	return buffer;
}

/*
 * other * limit / side rounded to the nearest pixel, at least 1 and at
 * most other: the proportional size of the side that does not limit.
 */
static uint32_t proportional(uint32_t other, uint32_t limit, uint32_t side)
{
	uint64_t result = ((uint64_t)other * limit + side / 2) / side;

	return result < 1 ? 1 : result > other ? other : (uint32_t)result;
}

/*
 * The size that fits width x height with the proportions kept, never
 * larger than the image: the limiting side takes the box's size exactly,
 * so that a 48 px icon becomes exactly 32 x 32.
 */
void fplinux_ui_image_fit_size(const lv_image_header_t *header, uint32_t width,
			       uint32_t height, uint32_t *fit_width,
			       uint32_t *fit_height)
{
	*fit_width = header->w;
	*fit_height = header->h;
	if (header->w <= width && header->h <= height)
		return;
	if ((uint64_t)header->w * height >= (uint64_t)header->h * width) {
		*fit_width = width;
		*fit_height = proportional(header->h, width, header->w);
	} else {
		*fit_height = height;
		*fit_width = proportional(header->w, height, header->h);
	}
}

/* The length both spans share, in units of 1 / (scaled size) pixel. */
static uint64_t overlap(uint64_t start, uint64_t end, uint64_t from,
			uint64_t to)
{
	uint64_t low = start > from ? start : from;
	uint64_t high = end < to ? end : to;

	return high > low ? high - low : 0;
}

/*
 * sum / divisor rounded to the nearest channel value; 0 without weight.
 * Both are below 2^53, so the double quotient is exact enough.
 */
static uint8_t average(uint64_t sum, uint64_t divisor)
{
	return divisor ? (uint8_t)((double)sum / (double)divisor + 0.5) : 0;
}

/*
 * Scales an ARGB8888 image or A8 mask down with an area filter: every pixel of the
 * result averages the source area it covers, weighted by coverage, with
 * premultiplied alpha so that transparent pixels add no colour.
 */
lv_draw_buf_t *fplinux_ui_image_area_scale(const lv_draw_buf_t *source,
					   uint32_t width, uint32_t height)
{
	uint32_t source_width = source->header.w;
	uint32_t source_height = source->header.h;
	uint64_t total = (uint64_t)source_width * source_height;
	bool mask = source->header.cf == LV_COLOR_FORMAT_A8;
	lv_draw_buf_t *result;

	if (!total || !width || !height)
		return NULL;
	result = lv_draw_buf_create(width, height, source->header.cf,
				    LV_STRIDE_AUTO);
	if (!result)
		return NULL;
	for (uint32_t y = 0; y < height; ++y) {
		uint8_t *out = result->data + y * result->header.stride;
		uint64_t top = (uint64_t)y * source_height;
		uint64_t bottom = top + source_height;

		for (uint32_t x = 0; x < width; ++x) {
			uint64_t left = (uint64_t)x * source_width;
			uint64_t right = left + source_width;
			uint64_t red = 0;
			uint64_t green = 0;
			uint64_t blue = 0;
			uint64_t alpha = 0;

			for (uint32_t sy = (uint32_t)(top / height);
			     sy < source_height &&
			     (uint64_t)sy * height < bottom;
			     ++sy) {
				const uint8_t *row = source->data +
						     sy * source->header.stride;
				uint64_t weight_y = overlap(
					top, bottom, (uint64_t)sy * height,
					(uint64_t)(sy + 1) * height);

				for (uint32_t sx = (uint32_t)(left / width);
				     sx < source_width &&
				     (uint64_t)sx * width < right;
				     ++sx) {
					uint64_t weight =
						weight_y *
						overlap(left, right,
							(uint64_t)sx * width,
							(uint64_t)(sx + 1) *
								width);
					lv_color32_t pixel =
						mask ?
							(lv_color32_t){
								.alpha = row[sx]
							} :
							((const lv_color32_t *)
								 row)[sx];
					uint64_t covered = weight * pixel.alpha;

					red += covered * pixel.red;
					green += covered * pixel.green;
					blue += covered * pixel.blue;
					alpha += covered;
				}
			}
			if (mask) {
				out[x] = average(alpha, total);
			} else {
				lv_color32_t *colour = (lv_color32_t *)out + x;

				colour->alpha = average(alpha, total);
				colour->red = average(red, alpha);
				colour->green = average(green, alpha);
				colour->blue = average(blue, alpha);
			}
		}
	}
	return result;
}

/* The alpha channel alone, which LVGL draws in the image_recolor colour. */
static lv_draw_buf_t *alpha_only(const lv_draw_buf_t *source)
{
	lv_draw_buf_t *mask =
		lv_draw_buf_create(source->header.w, source->header.h,
				   LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);

	if (!mask)
		return NULL;
	for (uint32_t y = 0; y < source->header.h; ++y) {
		const lv_color32_t *row =
			(const lv_color32_t *)(source->data +
					       y * source->header.stride);
		uint8_t *out = mask->data + y * mask->header.stride;

		for (uint32_t x = 0; x < source->header.w; ++x)
			out[x] = row[x].alpha;
	}
	return mask;
}

static bool is_symbolic(const char *path)
{
	size_t length = strlen(path);
	size_t suffix = strlen(SYMBOLIC_SUFFIX);

	return length >= suffix &&
	       !strcmp(path + length - suffix, SYMBOLIC_SUFFIX);
}

/*
 * Decodes an image source into a buffer that fits width x height, scaled
 * down with its proportions kept and never up; NULL with errno. A system
 * glyph, an alpha-only source or a "-symbolic.png" file gives an A8 mask.
 */
static lv_draw_buf_t *image_decode(const void *src, int width, int height,
				   bool alpha, bool preview)
{
	lv_image_dsc_t file_image;
	lv_image_header_t header;
	lv_draw_buf_t *buffer = NULL;
	uint8_t *data = NULL;
	uint32_t fit_width;
	uint32_t fit_height;

	if (width <= 0 || height <= 0) {
		errno = EINVAL;
		return NULL;
	}
	switch (src ? lv_image_src_get_type(src) : LV_IMAGE_SRC_UNKNOWN) {
	case LV_IMAGE_SRC_FILE:
		alpha = alpha || is_symbolic(src);
		data = read_png(src, &file_image);
		if (!data)
			return NULL;
		src = &file_image;
		break;
	case LV_IMAGE_SRC_VARIABLE:
		alpha = alpha || ((const lv_image_dsc_t *)src)->header.cf ==
					 LV_COLOR_FORMAT_A8;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}
	if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK ||
	    !header.w || !header.h) {
		errno = EINVAL;
	} else if ((uint64_t)header.w * header.h > IMAGE_MAX_PIXELS) {
		errno = EFBIG;
	} else {
		fplinux_ui_image_fit_size(&header, (uint32_t)width,
					  (uint32_t)height, &fit_width,
					  &fit_height);
		buffer = decode_image(src, &header, preview);
		if (buffer &&
		    (fit_width != header.w || fit_height != header.h)) {
			lv_draw_buf_t *full = buffer;

			buffer = fplinux_ui_image_area_scale(full, fit_width,
							     fit_height);
			lv_draw_buf_destroy(full);
			if (!buffer)
				errno = ENOMEM;
		}
	}
	free(data);
	if (buffer && alpha) {
		lv_draw_buf_t *mask = alpha_only(buffer);

		lv_draw_buf_destroy(buffer);
		buffer = mask;
		if (!buffer)
			errno = ENOMEM;
	}
	return buffer;
}

lv_draw_buf_t *fplinux_ui_image_decode(const void *src, int width, int height,
				       bool alpha)
{
	return image_decode(src, width, height, alpha, false);
}

lv_draw_buf_t *fplinux_ui_image_decode_preview(const lv_image_dsc_t *src,
					       int width, int height)
{
	const uint8_t *png = src->data;

	if (!png || src->data_size < 33 || src->data_size > 64U * 1024U ||
	    memcmp(png, png_signature, sizeof(png_signature)) ||
	    read_be32(png + 8) != 13 || memcmp(png + 12, "IHDR", 4) ||
	    !src->header.w || src->header.w > 128 || !src->header.h ||
	    src->header.h > 128 || read_be32(png + 16) != src->header.w ||
	    read_be32(png + 20) != src->header.h || png[24] > 8) {
		errno = EINVAL;
		return NULL;
	}
	return image_decode(src, width, height, false, true);
}

lv_draw_buf_t *fplinux_ui_image_load(const void *src, int width, int height)
{
	return fplinux_ui_image_decode(src, width, height, false);
}
