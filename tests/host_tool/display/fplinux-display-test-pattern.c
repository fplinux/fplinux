/* SPDX-License-Identifier: GPL-2.0-only */
#include "display-test-pattern.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 240U
#define HEIGHT 320U
#define STRIDE 484U
#define SIZE (HEIGHT * STRIDE)

static uint16_t pixel_at(const uint8_t *pixels, size_t stride, unsigned int x,
			 unsigned int y)
{
	const uint8_t *pixel = pixels + y * stride + x * 2;

	return (uint16_t)pixel[0] | (uint16_t)((uint16_t)pixel[1] << 8);
}

static void check_geometry(uint8_t *pixels)
{
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_GEOMETRY, 0));
	assert(pixel_at(pixels, STRIDE, 0, 0) == 0xf800);
	assert(pixel_at(pixels, STRIDE, 239, 0) == 0x07e0);
	assert(pixel_at(pixels, STRIDE, 0, 319) == 0x001f);
	assert(pixel_at(pixels, STRIDE, 239, 319) == 0xffe0);
	assert(pixel_at(pixels, STRIDE, 120, 0) == 0xf800);
	assert(pixel_at(pixels, STRIDE, 0, 160) == 0x07e0);
	assert(pixel_at(pixels, STRIDE, 239, 160) == 0xffff);
	assert(pixel_at(pixels, STRIDE, 120, 319) == 0x001f);
	assert(pixel_at(pixels, STRIDE, 30, 40) == 0xf800);
	assert(pixel_at(pixels, STRIDE, 31, 40) == 0x07e0);
	assert(pixel_at(pixels, STRIDE, 32, 40) == 0x001f);
	assert(pixel_at(pixels, STRIDE, 30, 120) == 0xf800);
	assert(pixel_at(pixels, STRIDE, 30, 121) == 0x07e0);
	assert(pixel_at(pixels, STRIDE, 30, 122) == 0x001f);
	assert(pixel_at(pixels, STRIDE, 30, 200) == 0xffff);
	assert(pixel_at(pixels, STRIDE, 31, 200) == 0x0000);
	assert(pixel_at(pixels, STRIDE, 180, 80) == 0x8410);
}

static void check_ids(uint8_t *pixels)
{
	static const unsigned int rows[] = { 20, 150, 280 };

	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_IDS, 5));
	for (unsigned int index = 0; index < 3; ++index) {
		unsigned int y = rows[index];

		/* Literal low bits 1,0,1 and high bit 0, each with its complement. */
		assert(pixel_at(pixels, STRIDE, 8, y) == 0xdefb);
		assert(pixel_at(pixels, STRIDE, 15, y) == 0x2104);
		assert(pixel_at(pixels, STRIDE, 22, y) == 0x2104);
		assert(pixel_at(pixels, STRIDE, 29, y) == 0xdefb);
		assert(pixel_at(pixels, STRIDE, 36, y) == 0xdefb);
		assert(pixel_at(pixels, STRIDE, 43, y) == 0x2104);
		assert(pixel_at(pixels, STRIDE, 218, y) == 0x2104);
		assert(pixel_at(pixels, STRIDE, 225, y) == 0xdefb);
	}
	assert(memcmp(pixels + 20 * STRIDE, pixels + 150 * STRIDE, WIDTH * 2) ==
	       0);
	assert(memcmp(pixels + 20 * STRIDE, pixels + 280 * STRIDE, WIDTH * 2) ==
	       0);
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_IDS, 32768));
	for (unsigned int index = 0; index < 3; ++index) {
		unsigned int y = rows[index];

		assert(pixel_at(pixels, STRIDE, 8, y) == 0x2104);
		assert(pixel_at(pixels, STRIDE, 218, y) == 0xdefb);
		assert(pixel_at(pixels, STRIDE, 225, y) == 0x2104);
	}
}

static void check_steady_gray_and_motion(uint8_t *pixels, uint8_t *previous)
{
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_GRAY, 0));
	assert(pixel_at(pixels, STRIDE, 0, 0) == 0x8410);
	assert(pixel_at(pixels, STRIDE, 239, 319) == 0x8410);
	memcpy(previous, pixels, SIZE);
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_GRAY, 65535));
	assert(memcmp(pixels, previous, SIZE) == 0);
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_MOTION, 5));
	assert(pixel_at(pixels, STRIDE, 39, 60) == 0xdefb);
	assert(pixel_at(pixels, STRIDE, 40, 60) == 0x2104);
	assert(pixel_at(pixels, STRIDE, 180, 80) == 0x8410);
	assert(pixel_at(pixels, STRIDE, 8, 20) == 0xdefb);
	assert(pixel_at(pixels, STRIDE, 8, 150) == 0xdefb);
	assert(pixel_at(pixels, STRIDE, 8, 280) == 0xdefb);
	assert(fplinux_display_test_draw(pixels, SIZE, STRIDE, WIDTH, HEIGHT,
					 FPLINUX_DISPLAY_TEST_MOTION, 6));
	assert(pixel_at(pixels, STRIDE, 47, 60) == 0xdefb);
	assert(pixel_at(pixels, STRIDE, 48, 60) == 0x2104);
	assert(pixel_at(pixels, STRIDE, 180, 80) == 0x8410);
}

static void check_odd_width_and_pitch(void)
{
	size_t size = 320 * 485;
	uint8_t *allocation = malloc(size + 2);
	uint8_t *pixels;

	assert(allocation);
	memset(allocation, 0xa5, size + 2);
	pixels = allocation + 1;
	assert(fplinux_display_test_draw(pixels, size, 485, 241, 320,
					 FPLINUX_DISPLAY_TEST_GEOMETRY, 0));
	assert(pixel_at(pixels, 485, 0, 0) == 0xf800);
	assert(pixel_at(pixels, 485, 240, 319) == 0xffe0);
	assert(allocation[0] == 0xa5 && allocation[size + 1] == 0xa5);
	for (unsigned int y = 0; y < 320; ++y)
		for (unsigned int byte = 482; byte < 485; ++byte)
			assert(pixels[y * 485 + byte] == 0xa5);
	errno = 0;
	assert(!fplinux_display_test_draw(pixels, size - 1, 485, 241, 320,
					  FPLINUX_DISPLAY_TEST_GRAY, 0));
	assert(errno == EINVAL);
	assert(pixel_at(pixels, 485, 0, 0) == 0xf800);
	assert(pixel_at(pixels, 485, 240, 319) == 0xffe0);
	free(allocation);
}

int main(void)
{
	uint8_t *pixels = malloc(SIZE);
	uint8_t *previous = malloc(SIZE);

	assert(pixels && previous);
	memset(pixels, 0xa5, SIZE);
	check_geometry(pixels);
	check_ids(pixels);
	check_steady_gray_and_motion(pixels, previous);
	for (unsigned int y = 0; y < HEIGHT; ++y)
		for (unsigned int byte = WIDTH * 2; byte < STRIDE; ++byte)
			assert(pixels[y * STRIDE + byte] == 0xa5);
	check_odd_width_and_pitch();
	free(previous);
	free(pixels);
	return 0;
}
