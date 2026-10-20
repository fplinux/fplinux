/* SPDX-License-Identifier: GPL-2.0-only */
#include "display-test-pattern.h"

#include <errno.h>

#define GRAY 0x8410U
#define DARK 0x2104U
#define LIGHT 0xdefbU

struct canvas {
	uint8_t *pixels;
	size_t stride;
	uint32_t width;
	uint32_t height;
};

static void put_pixel(uint8_t *output, uint16_t pixel)
{
	output[0] = (uint8_t)pixel;
	output[1] = (uint8_t)(pixel >> 8);
}

static void fill_rect(const struct canvas *canvas, uint32_t left, uint32_t top,
		      uint32_t right, uint32_t bottom, uint16_t pixel)
{
	for (uint32_t y = top; y < bottom; ++y) {
		uint8_t *output =
			canvas->pixels + y * canvas->stride + (size_t)left * 2;

		for (uint32_t x = left; x < right; ++x, output += 2)
			put_pixel(output, pixel);
	}
}

static uint32_t fraction(uint32_t extent, unsigned int numerator,
			 unsigned int denominator)
{
	return (uint32_t)((uint64_t)extent * numerator / denominator);
}

static void draw_geometry(const struct canvas *canvas)
{
	static const uint16_t colors[] = { 0xf800, 0x07e0, 0x001f };
	uint32_t left = canvas->width / 8;
	uint32_t right = fraction(canvas->width, 7, 8);
	uint32_t top = canvas->height / 8;
	uint32_t bottom = fraction(canvas->height, 7, 8);
	uint32_t second = top + (bottom - top) / 3;
	uint32_t third = top + fraction(bottom - top, 2, 3);
	uint32_t corner_width = canvas->width / 10 + 1;
	uint32_t corner_height = canvas->height / 10 + 1;

	for (uint32_t y = top; y < bottom; ++y) {
		uint8_t *output =
			canvas->pixels + y * canvas->stride + (size_t)left * 2;

		for (uint32_t x = left; x < right; ++x, output += 2) {
			uint16_t pixel;

			if (y < second)
				pixel = colors[(x - left) % 3];
			else if (y < third)
				pixel = colors[(y - second) % 3];
			else
				pixel = (x ^ y) & 1U ? 0 : 0xffff;
			put_pixel(output, pixel);
		}
	}
	fill_rect(canvas, 0, 0, canvas->width, 1, 0xf800);
	fill_rect(canvas, 0, canvas->height - 1, canvas->width, canvas->height,
		  0x001f);
	fill_rect(canvas, 0, 0, 1, canvas->height, 0x07e0);
	fill_rect(canvas, canvas->width - 1, 0, canvas->width, canvas->height,
		  0xffff);
	fill_rect(canvas, 0, 0, corner_width, corner_height, 0xf800);
	fill_rect(canvas, canvas->width - corner_width, 0, canvas->width,
		  corner_height, 0x07e0);
	fill_rect(canvas, 0, canvas->height - corner_height, corner_width,
		  canvas->height, 0x001f);
	fill_rect(canvas, canvas->width - corner_width,
		  canvas->height - corner_height, canvas->width, canvas->height,
		  0xffe0);
}

static void draw_ids(const struct canvas *canvas, uint32_t frame_id)
{
	uint32_t cell_width = (canvas->width / 16) & ~1U;
	uint32_t left = (canvas->width - cell_width * 16) / 2;
	uint32_t tops[] = { canvas->height / 16,
			    fraction(canvas->height, 15, 32),
			    fraction(canvas->height, 7, 8) };
	uint32_t marker_height = canvas->height / 16;

	/* Each bit exchanges two equal areas, keeping marker brightness steady. */
	for (unsigned int row = 0; row < 3; ++row) {
		for (unsigned int bit = 0; bit < 16; ++bit) {
			uint32_t start = left + bit * cell_width;
			uint32_t middle = start + cell_width / 2;
			uint32_t end = start + cell_width;
			bool set = (frame_id & (1U << bit)) != 0;

			fill_rect(canvas, start, tops[row], middle,
				  tops[row] + marker_height,
				  set ? LIGHT : DARK);
			fill_rect(canvas, middle, tops[row], end,
				  tops[row] + marker_height,
				  set ? DARK : LIGHT);
		}
	}
}

bool fplinux_display_test_draw(uint8_t *pixels, size_t size, size_t stride,
			       uint32_t width, uint32_t height,
			       enum fplinux_display_test_pattern pattern,
			       uint32_t frame_id)
{
	struct canvas canvas = {
		.pixels = pixels,
		.stride = stride,
		.width = width,
		.height = height,
	};

	if (!pixels || !width || !height || SIZE_MAX / width < 2 ||
	    stride < (size_t)width * 2 || stride > size / height ||
	    (unsigned int)pattern > FPLINUX_DISPLAY_TEST_MOTION) {
		errno = EINVAL;
		return false;
	}
	fill_rect(&canvas, 0, 0, width, height,
		  pattern == FPLINUX_DISPLAY_TEST_MOTION ? DARK : GRAY);
	switch (pattern) {
	case FPLINUX_DISPLAY_TEST_GEOMETRY:
		draw_geometry(&canvas);
		break;
	case FPLINUX_DISPLAY_TEST_GRAY:
		return true;
	case FPLINUX_DISPLAY_TEST_MOTION:
		fill_rect(&canvas, 0, 0,
			  (uint32_t)((uint64_t)frame_id * 8 % width), height,
			  LIGHT);
		/* Camera aliasing and rolling shutter can split the moving edge. */
		/* fall through */
	case FPLINUX_DISPLAY_TEST_IDS:
		draw_ids(&canvas, frame_id);
		break;
	}
	/* This fixed gray area is the brightness reference in every pattern. */
	fill_rect(&canvas, fraction(width, 3, 4), height / 4,
		  fraction(width, 15, 16), fraction(height, 3, 8), GRAY);
	return true;
}
