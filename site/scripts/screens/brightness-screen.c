/* SPDX-License-Identifier: GPL-2.0-only */
/* Render the production brightness screen at one level and save the frame. */
#include "brightness-ui.h"
#include "screen-frame.h"

#include <stdio.h>
#include <stdlib.h>

#define SCREEN_WIDTH 240U
#define SCREEN_HEIGHT 320U

int main(int argc, char **argv)
{
	static uint16_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
	struct brightness_ui_surface surface = {
		.pixels = pixels,
		.width = SCREEN_WIDTH,
		.height = SCREEN_HEIGHT,
		.stride_bytes = SCREEN_WIDTH * sizeof(pixels[0]),
	};
	char *end;
	unsigned long level;
	bool rendered;

	if (argc != 3) {
		fprintf(stderr, "usage: %s LEVEL OUTPUT\n", argv[0]);
		return EXIT_FAILURE;
	}
	level = strtoul(argv[1], &end, 10);
	if (*end) {
		fprintf(stderr, "invalid brightness level %s\n", argv[1]);
		return EXIT_FAILURE;
	}
	rendered = brightness_ui_render(&surface, (unsigned int)level, false);
	if (!rendered) {
		fprintf(stderr, "cannot render brightness level %s\n", argv[1]);
		return EXIT_FAILURE;
	}
	return screen_frame_write(argv[2], pixels, surface.width,
				  surface.height, surface.stride_bytes) ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
