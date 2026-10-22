/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void expect_shape(const lv_draw_buf_t *buffer, uint32_t width,
			 uint32_t height, lv_color_format_t format)
{
	assert(buffer);
	assert(buffer->header.w == width);
	assert(buffer->header.h == height);
	assert(buffer->header.cf == format);
}

static void expect_pixels(const lv_draw_buf_t *buffer, uint32_t width,
			  uint32_t height, const lv_color32_t *expected)
{
	expect_shape(buffer, width, height, LV_COLOR_FORMAT_ARGB8888);
	for (uint32_t y = 0; y < height; ++y) {
		const lv_color32_t *row =
			(const lv_color32_t *)(buffer->data +
					       y * buffer->header.stride);

		for (uint32_t x = 0; x < width; ++x) {
			lv_color32_t pixel = expected[y * width + x];

			assert(row[x].red == pixel.red);
			assert(row[x].green == pixel.green);
			assert(row[x].blue == pixel.blue);
			assert(row[x].alpha == pixel.alpha);
		}
	}
}

static void expect_mask(const lv_draw_buf_t *buffer, uint32_t width,
			uint32_t height, const uint8_t *expected)
{
	expect_shape(buffer, width, height, LV_COLOR_FORMAT_A8);
	for (uint32_t y = 0; y < height; ++y)
		assert(!memcmp(buffer->data + y * buffer->header.stride,
			       expected + y * width, width));
}

static void expect_transparent(const lv_draw_buf_t *buffer, uint32_t width,
			       uint32_t height, bool mask)
{
	expect_shape(buffer, width, height,
		     mask ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *row = buffer->data + y * buffer->header.stride;

		for (uint32_t x = 0; x < width; ++x)
			assert(mask ? !row[x] :
				      !((const lv_color32_t *)row)[x].alpha);
	}
}

static void test_transparent_png(const char *path, bool symbolic)
{
	lv_draw_buf_t *buffer = fplinux_ui_image_load(path, 20, 20);

	expect_transparent(buffer, 2, 2, symbolic);
	lv_draw_buf_destroy(buffer);
}

static void test_a8(bool transparent)
{
	uint8_t pixels[] = { 0, 128, 255, 64 };
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_A8,
			.w = 2,
			.h = 2,
			.stride = 2,
		},
		.data_size = sizeof(pixels),
		.data = pixels,
	};
	const uint8_t expected[] = { 0, 128, 255, 64 };
	lv_draw_buf_t *buffer;

	if (transparent)
		memset(pixels, 0, sizeof(pixels));
	buffer = fplinux_ui_image_load(&source, 20, 20);
	memset(pixels, 255, sizeof(pixels));
	if (transparent)
		expect_transparent(buffer, 2, 2, true);
	else
		expect_mask(buffer, 2, 2, expected);
	lv_draw_buf_destroy(buffer);
}

static void test_transparent_downscale(const char *path)
{
	lv_draw_buf_t *buffer = fplinux_ui_image_load(path, 1, 1);

	expect_transparent(buffer, 1, 1, false);
	lv_draw_buf_destroy(buffer);
}

static void test_area_scale(void)
{
	lv_color32_t pixels[] = {
		{ .red = 255, .alpha = 255 },
		{ .green = 255, .alpha = 255 },
		{ .blue = 255, .alpha = 255 },
	};
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_ARGB8888,
			.w = 3,
			.h = 1,
			.stride = sizeof(pixels),
		},
		.data_size = sizeof(pixels),
		.data = (const uint8_t *)pixels,
	};
	const lv_color32_t expected[] = {
		{ .red = 170, .green = 85, .alpha = 255 },
		{ .green = 85, .blue = 170, .alpha = 255 },
	};
	lv_draw_buf_t *scaled = fplinux_ui_image_load(&source, 2, 2);
	lv_draw_buf_t *full = fplinux_ui_image_load(&source, 20, 20);

	expect_pixels(scaled, 2, 1, expected);
	expect_pixels(full, 3, 1, pixels);
	memset(pixels, 0, sizeof(pixels));
	expect_pixels(scaled, 2, 1, expected);
	lv_draw_buf_destroy(full);
	lv_draw_buf_destroy(scaled);
}

static void test_owned_file(const char *path)
{
	const lv_color32_t expected[] = {
		{ .red = 255, .alpha = 255 },
		{ .blue = 255, .alpha = 255 },
	};
	lv_draw_buf_t *loaded = fplinux_ui_image_load(path, 20, 20);
	lv_draw_buf_t *copy;

	assert(loaded);
	assert(!unlink(path));
	copy = fplinux_ui_image_load(loaded, 20, 20);
	lv_draw_buf_destroy(loaded);
	expect_pixels(copy, 2, 1, expected);
	lv_draw_buf_destroy(copy);
}

static void test_symbolic_mask(const char *path)
{
	const uint8_t expected[] = { 255, 128 };
	lv_draw_buf_t *buffer = fplinux_ui_image_load(path, 20, 20);

	expect_mask(buffer, 2, 1, expected);
	lv_draw_buf_destroy(buffer);
}

static void test_file_error(const char *path, int error)
{
	errno = 0;
	assert(!fplinux_ui_image_load(path, 20, 20));
	assert(errno == error);
}

static void test_invalid_arguments(void)
{
	const uint8_t pixels[] = { 255 };
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_A8,
			.w = 1,
			.h = 1,
			.stride = 1,
		},
		.data_size = sizeof(pixels),
		.data = pixels,
	};

	assert(!fplinux_ui_image_load(&source, 0, 1) && errno == EINVAL);
	assert(!fplinux_ui_image_load(&source, 1, -1) && errno == EINVAL);
	assert(!fplinux_ui_image_load(NULL, 1, 1) && errno == EINVAL);
}

int main(int argc, char **argv)
{
	lv_display_t *display;
	const char *path;

	if (argc < 2 || argc > 3)
		return 2;
	path = argc == 3 ? argv[2] : NULL;
	lv_init();
	display = lv_display_create(32, 32);
	assert(display);
	if (!strcmp(argv[1], "transparent-png"))
		test_transparent_png(path, false);
	else if (!strcmp(argv[1], "transparent-symbolic"))
		test_transparent_png(path, true);
	else if (!strcmp(argv[1], "transparent-a8"))
		test_a8(true);
	else if (!strcmp(argv[1], "a8-mask"))
		test_a8(false);
	else if (!strcmp(argv[1], "transparent-downscale"))
		test_transparent_downscale(path);
	else if (!strcmp(argv[1], "area-scale"))
		test_area_scale();
	else if (!strcmp(argv[1], "owned-file"))
		test_owned_file(path);
	else if (!strcmp(argv[1], "symbolic-mask"))
		test_symbolic_mask(path);
	else if (!strcmp(argv[1], "invalid-png"))
		test_file_error(path, EINVAL);
	else if (!strcmp(argv[1], "file-limit") ||
		 !strcmp(argv[1], "pixel-limit"))
		test_file_error(path, EFBIG);
	else if (!strcmp(argv[1], "invalid-arguments"))
		test_invalid_arguments();
	else
		return 2;
	lv_display_delete(display);
	lv_deinit();
	printf("%s: ok\n", argv[1]);
	return 0;
}
