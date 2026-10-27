/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PHONEME_TEST_GXJ_PUTPIXEL_H
#define FPLINUX_PHONEME_TEST_GXJ_PUTPIXEL_H

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t gxj_pixel_type;
typedef struct {
	int width, height;
	gxj_pixel_type *pixelData;
	unsigned char *alphaData;
} gxj_screen_buffer;

#define IMGAPI_GET_IMAGEDATA_PTR(image) (image)

/* This shell boundary never supplies Java image objects. */
static inline bool gxj_get_image_screen_buffer_impl(void *image,
						    gxj_screen_buffer *buffer,
						    void *pixels)
{
	(void)image;
	(void)buffer;
	(void)pixels;
	return false;
}

#endif
