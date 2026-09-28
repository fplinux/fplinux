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

static int write_ppm(const char *directory, unsigned int width,
		     unsigned int height, const uint16_t *pixels)
{
	char path[512];
	FILE *file;

	if (snprintf(path, sizeof(path), "%s/%ux%u.ppm", directory, width,
		     height) >= (int)sizeof(path))
		return 1;
	file = fopen(path, "wb");
	if (!file)
		return 1;
	if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0)
		goto fail;
	for (size_t index = 0; index < (size_t)width * height; ++index) {
		uint16_t pixel = pixels[index];
		unsigned char rgb[3] = {
			(unsigned char)(((pixel >> 11U) & 31U) * 255U / 31U),
			(unsigned char)(((pixel >> 5U) & 63U) * 255U / 63U),
			(unsigned char)((pixel & 31U) * 255U / 31U),
		};

		if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb))
			goto fail;
	}
	return fclose(file) == 0 ? 0 : 1;
fail:
	fclose(file);
	return 1;
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

	for (unsigned int row = surface->height / 2U;
	     row < surface->height * 3U / 4U; ++row) {
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

static int check_render(const char *directory, unsigned int width,
			unsigned int height)
{
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
	if (!brightness_ui_render(&surface, 0U, false))
		goto done;
	if (maximum_bright_runs(&surface) != 0U)
		goto done;
	if (!brightness_ui_render(&surface, 5U, false))
		goto done;
	if (maximum_bright_runs(&surface) != 5U)
		goto done;
	if (write_ppm(directory, width, height, pixels))
		goto done;
	if (!brightness_ui_render(&surface, 10U, true))
		goto done;
	if (maximum_bright_runs(&surface) != 10U)
		goto done;
	result = 0;
done:
	free(pixels);
	return result;
}

int main(int argc, char **argv)
{
	if (argc != 2 || check_keys() || check_render(argv[1], 128U, 160U) ||
	    check_render(argv[1], 240U, 320U)) {
		fprintf(stderr, "brightness UI host behavior failed\n");
		return 1;
	}
	return 0;
}
