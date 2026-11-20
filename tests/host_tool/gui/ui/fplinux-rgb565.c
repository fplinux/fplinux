/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-rgb565.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_rotation(void)
{
	const uint16_t source[] = {
		0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0xffe0,
	};
	const uint16_t clockwise[] = {
		0xffff, 0xf800, 0x0000, 0x07e0, 0xffe0, 0x001f,
	};
	const uint16_t counterclockwise[] = {
		0x001f, 0xffe0, 0x07e0, 0x0000, 0xf800, 0xffff,
	};
	struct fplinux_rgb565_viewport viewport;
	uint16_t output[6];
	int x, y;

	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 2, 3, 2,
					     FPLINUX_RGB565_ONE_TO_ONE,
					     FPLINUX_RGB565_ROTATE_0));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 3));
	assert(!memcmp(output, source, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 2, 1, &x, &y) == 1);
	assert(x == 2 && y == 1);

	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 2, 2, 3,
					     FPLINUX_RGB565_ONE_TO_ONE,
					     FPLINUX_RGB565_ROTATE_90));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 2));
	assert(!memcmp(output, clockwise, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 0, 2, &x, &y) == 1);
	assert(x == 2 && y == 1);

	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 2, 2, 3,
					     FPLINUX_RGB565_ONE_TO_ONE,
					     FPLINUX_RGB565_ROTATE_270));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 2));
	assert(!memcmp(output, counterclockwise, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 0, 2, &x, &y) == 1);
	assert(x == 0 && y == 0);
}

static void test_enlargement_and_margins(void)
{
	const uint16_t source[] = { 0xf800, 0x001f, 0x9999 };
	const uint16_t expected[] = {
		0xf800, 0xf800, 0x001f, 0x001f, 0xaaaa, 0xf800, 0xf800, 0x001f,
		0x001f, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
	};
	uint16_t output[15];
	struct fplinux_rgb565_viewport viewport;
	int x = -7, y = -9;

	for (size_t i = 0; i < 15; ++i)
		output[i] = 0xaaaa;
	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 1, 4, 3,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_0));
	assert(viewport.x == 0 && viewport.y == 0);
	assert(viewport.width == 4 && viewport.height == 2);
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 5));
	assert(!memcmp(output, expected, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 3, 1, &x, &y) == 1);
	assert(x == 1 && y == 0);
	assert(fplinux_rgb565_map_point(&viewport, 0, 2, &x, &y) == 0);
	assert(x == 1 && y == 0);
}

static void test_original_size_preserves_margins_and_stride(void)
{
	const uint16_t source[] = {
		0xf800, 0x07e0, 0xbeef, 0x001f, 0xffff, 0xbeef,
	};
	const uint16_t expected[] = {
		0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0xf800,
		0x07e0, 0x1234, 0x1234, 0x1234, 0x001f, 0xffff, 0x1234,
		0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0x1234,
	};
	uint16_t output[20];
	struct fplinux_rgb565_viewport viewport;

	for (size_t i = 0; i < 20; ++i)
		output[i] = 0x1234;
	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 2, 4, 4,
					     FPLINUX_RGB565_ONE_TO_ONE,
					     FPLINUX_RGB565_ROTATE_0));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 5));
	assert(!memcmp(output, expected, sizeof(output)));
}

static void test_rotated_enlargement(void)
{
	const uint16_t source[] = { 0xf800, 0x001f };
	const uint16_t clockwise[] = {
		0xf800, 0xf800, 0xf800, 0xf800, 0x001f, 0x001f, 0x001f, 0x001f,
	};
	const uint16_t counterclockwise[] = {
		0x001f, 0x001f, 0x001f, 0x001f, 0xf800, 0xf800, 0xf800, 0xf800,
	};
	struct fplinux_rgb565_viewport viewport;
	uint16_t output[8];
	int x, y;

	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 1, 2, 4,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_90));
	assert(!fplinux_rgb565_transform(&viewport, source, 2, output, 2));
	assert(!memcmp(output, clockwise, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 1, 3, &x, &y) == 1);
	assert(x == 1 && y == 0);
	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 1, 2, 4,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_270));
	assert(!fplinux_rgb565_transform(&viewport, source, 2, output, 2));
	assert(!memcmp(output, counterclockwise, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 1, 3, &x, &y) == 1);
	assert(x == 0 && y == 0);
}

static void test_area_reduction(void)
{
	const uint16_t source[] = { 0xf800, 0x07e0, 0x001f };
	const uint16_t square[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
	const uint16_t expected[] = { 0xaaa0, 0x02b5 };
	const uint16_t reversed[] = { 0x02b5, 0xaaa0 };
	struct fplinux_rgb565_viewport viewport;
	uint16_t output[2];
	int x, y;

	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 1, 2, 1,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_0));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 2));
	assert(!memcmp(output, expected, sizeof(output)));
	assert(fplinux_rgb565_map_point(&viewport, 1, 0, &x, &y) == 1);
	assert(x == 1 && y == 0);

	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 1, 1, 2,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_90));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 1));
	assert(!memcmp(output, expected, sizeof(output)));
	assert(!fplinux_rgb565_viewport_init(&viewport, 3, 1, 1, 2,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_270));
	assert(!fplinux_rgb565_transform(&viewport, source, 3, output, 1));
	assert(!memcmp(output, reversed, sizeof(output)));
	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 2, 1, 1,
					     FPLINUX_RGB565_FIT,
					     FPLINUX_RGB565_ROTATE_0));
	assert(!fplinux_rgb565_transform(&viewport, square, 2, output, 1));
	assert(output[0] == 0x8410);
}

static void test_integer_scale(void)
{
	const uint16_t source[] = {
		0xf800, 0x07e0, 0x001f, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffff,
	};
	const uint16_t expected[] = { 0x8400, 0x841f };
	struct fplinux_rgb565_viewport viewport;
	uint16_t output[2];

	assert(!fplinux_rgb565_viewport_init(&viewport, 4, 2, 2, 2,
					     FPLINUX_RGB565_INTEGER,
					     FPLINUX_RGB565_ROTATE_0));
	assert(viewport.x == 0 && viewport.y == 0);
	assert(viewport.width == 2 && viewport.height == 1);
	assert(!fplinux_rgb565_transform(&viewport, source, 4, output, 2));
	assert(!memcmp(output, expected, sizeof(output)));
	assert(!fplinux_rgb565_viewport_init(&viewport, 2, 3, 7, 8,
					     FPLINUX_RGB565_INTEGER,
					     FPLINUX_RGB565_ROTATE_0));
	assert(viewport.x == 1 && viewport.y == 1);
	assert(viewport.width == 4 && viewport.height == 6);
}

static void test_half_size_rounds_channels_and_preserves_margins(void)
{
	const uint16_t source[] = {
		0xf800, 0x07e0, 0x0861, 0x0000, 0x07e0, 0x07e0, 0xbeef,
		0x001f, 0xffff, 0x0021, 0x0000, 0x07e0, 0x07e0, 0xbeef,
		0x0000, 0x0000, 0x0000, 0x0000, 0xffff, 0xffff, 0xbeef,
		0x0000, 0x0000, 0x0000, 0x1061, 0xffff, 0xffff, 0xbeef,
	};
	const uint16_t expected[][6] = {
		{ 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa },
		{ 0xaaaa, 0x8410, 0x0021, 0x07e0, 0xaaaa, 0xaaaa },
		{ 0xaaaa, 0x0000, 0x0820, 0xffff, 0xaaaa, 0xaaaa },
		{ 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa },
	};
	uint16_t output[sizeof(expected) / sizeof(expected[0][0])];
	struct fplinux_rgb565_viewport viewport;

	for (size_t i = 0; i < sizeof(output) / sizeof(*output); ++i)
		output[i] = 0xaaaa;
	assert(!fplinux_rgb565_viewport_init(&viewport, 6, 4, 5, 4,
					     FPLINUX_RGB565_INTEGER,
					     FPLINUX_RGB565_ROTATE_0));
	assert(!fplinux_rgb565_transform(&viewport, source, 7, output, 6));
	assert(!memcmp(output, &expected, sizeof(output)));
}

static void test_clipping_and_invalid_input(void)
{
	const uint16_t source[] = {
		1, 2, 3, 4, 99, 5, 6, 7, 8, 99,
	};
	const uint16_t expected[] = { 6, 7, 99 };
	struct fplinux_rgb565_viewport viewport, saved;
	uint16_t output[] = { 99, 99, 99 };
	int x, y;

	assert(!fplinux_rgb565_viewport_init(&viewport, 4, 2, 2, 1,
					     FPLINUX_RGB565_ONE_TO_ONE,
					     FPLINUX_RGB565_ROTATE_0));
	assert(viewport.x == -1 && viewport.y == -1);
	assert(fplinux_rgb565_map_point(&viewport, 0, 0, &x, &y) == 1);
	assert(x == 1 && y == 1);
	assert(fplinux_rgb565_map_point(&viewport, -1, 0, &x, &y) == 0);
	assert(!fplinux_rgb565_transform(&viewport, source, 5, output, 3));
	assert(!memcmp(output, expected, sizeof(output)));
	saved = viewport;
	assert(fplinux_rgb565_viewport_init(&viewport, 4097, 2, 2, 1,
					    FPLINUX_RGB565_FIT,
					    FPLINUX_RGB565_ROTATE_0) == -1);
	assert(viewport.source_width == saved.source_width &&
	       viewport.source_height == saved.source_height &&
	       viewport.destination_width == saved.destination_width &&
	       viewport.destination_height == saved.destination_height &&
	       viewport.x == saved.x && viewport.y == saved.y &&
	       viewport.width == saved.width &&
	       viewport.height == saved.height &&
	       viewport.rotation == saved.rotation);
	assert(fplinux_rgb565_transform(&viewport, source, 3, output, 3) == -1);
	assert(!memcmp(output, expected, sizeof(output)));
}

int main(void)
{
	test_rotation();
	test_enlargement_and_margins();
	test_original_size_preserves_margins_and_stride();
	test_rotated_enlargement();
	test_area_reduction();
	test_integer_scale();
	test_half_size_rounds_channels_and_preserves_margins();
	test_clipping_and_invalid_input();
	return 0;
}
