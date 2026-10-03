/* SPDX-License-Identifier: GPL-2.0-only */
#include "brightness-ui.h"
#include "fplinux-keypad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_keys(void)
{
	unsigned int requested = 99U;

	if (brightness_ui_key(0U, FPLINUX_KEY_DOWN, &requested) !=
		    BRIGHTNESS_UI_NONE ||
	    requested != 99U)
		return 1;
	if (brightness_ui_key(0U, FPLINUX_KEY_UP, &requested) !=
		    BRIGHTNESS_UI_SET ||
	    requested != 1U)
		return 1;
	if (brightness_ui_key(10U, FPLINUX_KEY_UP, &requested) !=
		    BRIGHTNESS_UI_NONE ||
	    requested != 1U)
		return 1;
	if (brightness_ui_key(10U, FPLINUX_KEY_DOWN, &requested) !=
		    BRIGHTNESS_UI_SET ||
	    requested != 9U)
		return 1;
	if (brightness_ui_key(5U, FPLINUX_KEY_SOFT_RIGHT, &requested) !=
	    BRIGHTNESS_UI_EXIT)
		return 1;
	if (brightness_ui_key(5U, FPLINUX_KEY_LEFT, &requested) !=
	    BRIGHTNESS_UI_NONE)
		return 1;
	return 0;
}

static bool is_bright(uint16_t pixel)
{
	unsigned int red = (pixel >> 11U) & 31U;
	unsigned int green = (pixel >> 5U) & 63U;
	unsigned int blue = pixel & 31U;

	return red > 20U && green > 40U && blue > 20U;
}

static unsigned int
maximum_bright_runs(const struct brightness_ui_surface *surface)
{
	unsigned int maximum = 0;

	for (unsigned int row = surface->height * 62U / 100U;
	     row < surface->height * 62U / 100U + surface->height / 22U;
	     ++row) {
		unsigned int runs = 0;
		bool in_run = false;

		for (unsigned int column = 0; column < surface->width;
		     ++column) {
			uint16_t pixel =
				surface->pixels[(size_t)row * surface->width +
						column];

			if (is_bright(pixel) && !in_run)
				++runs;
			in_run = is_bright(pixel);
		}
		if (runs > maximum)
			maximum = runs;
	}
	return maximum;
}

static size_t color_count(const struct brightness_ui_surface *surface,
			  unsigned int first_row, unsigned int last_row,
			  uint16_t color)
{
	size_t count = 0;

	for (unsigned int row = first_row; row < last_row; ++row)
		for (unsigned int column = 0; column < surface->width; ++column)
			if (surface->pixels[(size_t)row * surface->width +
					    column] == color)
				++count;
	return count;
}

static int check_render(unsigned int width, unsigned int height,
			const char *font_path)
{
	struct fplinux_font font = { 0 };
	uint16_t *pixels = calloc((size_t)width * height, sizeof(*pixels));
	struct brightness_ui_surface surface = {
		.pixels = pixels,
		.width = width,
		.height = height,
		.stride_bytes = (size_t)width * sizeof(*pixels),
	};
	int result = 1;

	if (!pixels)
		return 1;
	if (!fplinux_font_open(&font, font_path))
		goto done;
	if (!brightness_ui_render(&surface, &font, 0U, false))
		goto done;
	if (maximum_bright_runs(&surface) != 0U)
		goto done;
	if (!brightness_ui_render(&surface, &font, 5U, false))
		goto done;
	if (maximum_bright_runs(&surface) != 5U)
		goto done;
	/* Solid fixture glyphs must fit completely, including the exit hint. */
	if (color_count(&surface, 0U, height / 4U, 0x6b4dU) !=
		    (width == 128U ? 2880U : 5120U) ||
	    color_count(&surface, height * 88U / 100U, height, 0x6b4dU) !=
		    (width == 128U ? 936U : 1664U))
		goto done;
	if (!brightness_ui_render(&surface, &font, 10U, true))
		goto done;
	if (maximum_bright_runs(&surface) != 10U)
		goto done;
	result = 0;
done:
	fplinux_font_close(&font);
	free(pixels);
	return result;
}

int main(int argc, char **argv)
{
	static const unsigned int sizes[][2] = { { 128U, 160U },
						 { 240U, 320U } };

	if (argc != 3) {
		fprintf(stderr, "brightness UI host fonts required\n");
		return 1;
	}
	if (check_keys()) {
		fprintf(stderr, "brightness UI keys failed\n");
		return 1;
	}
	for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]);
	     ++index) {
		if (check_render(sizes[index][0], sizes[index][1],
				 argv[index + 1U])) {
			fprintf(stderr, "brightness UI render %ux%u failed\n",
				sizes[index][0], sizes[index][1]);
			return 1;
		}
	}
	return 0;
}
