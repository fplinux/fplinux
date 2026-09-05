// SPDX-License-Identifier: GPL-2.0-only
/* Render the production ARMADA scene as deterministic RGB24 host frames. */
#include "../../alpine/aports/fplinux-showcase/armada-scene.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PREVIEW_WIDTH
#define PREVIEW_WIDTH 240U
#endif
#ifndef PREVIEW_HEIGHT
#define PREVIEW_HEIGHT 320U
#endif

static uint8_t expand5(unsigned int value)
{
	return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand6(unsigned int value)
{
	return (uint8_t)((value << 2) | (value >> 4));
}

static void indicator(uint16_t *pixels, unsigned int x, uint16_t color,
		      bool enabled)
{
	unsigned int dx;
	unsigned int dy;

	for (dy = 0; dy < 6U; ++dy)
		for (dx = 0; dx < 18U; ++dx)
			pixels[(PREVIEW_HEIGHT - 8U + dy) * PREVIEW_WIDTH + x +
			       dx] = enabled ? color : 0x2104;
}

int main(void)
{
	uint16_t *pixels =
		calloc((size_t)PREVIEW_WIDTH * PREVIEW_HEIGHT, sizeof(*pixels));
	uint8_t *rgb = malloc((size_t)PREVIEW_WIDTH * PREVIEW_HEIGHT * 3U);
	struct armada_scene *scene;
	struct armada_metrics metrics = {
		.average_fps_tenths = 300,
		.minimum_fps_tenths = 300,
		.maximum_frame_us = 33333,
		.valid = true,
	};
	uint32_t frame;

	if (!pixels || !rgb) {
		fprintf(stderr, "fplinux-showcase-preview: out of memory\n");
		free(rgb);
		free(pixels);
		return EXIT_FAILURE;
	}
	scene = armada_scene_create(PREVIEW_WIDTH, PREVIEW_HEIGHT, pixels);
	if (!scene) {
		fprintf(stderr, "fplinux-showcase-preview: scene: %s\n",
			strerror(errno));
		free(rgb);
		free(pixels);
		return EXIT_FAILURE;
	}
	for (frame = 0; frame < ARMADA_DURATION_FRAMES; ++frame) {
		struct armada_outputs outputs;
		size_t index;

		armada_scene_render(scene, frame, &metrics, &outputs);
		indicator(pixels, 8U, 0xffe0, outputs.keypad);
		indicator(pixels, 32U, 0xf81f, outputs.rumble);
		indicator(pixels, 56U, 0x07ff, outputs.lcd_level >= 0);
		for (index = 0; index < (size_t)PREVIEW_WIDTH * PREVIEW_HEIGHT;
		     ++index) {
			uint16_t pixel = pixels[index];

			rgb[index * 3U] = expand5((pixel >> 11) & 0x1fU);
			rgb[index * 3U + 1U] = expand6((pixel >> 5) & 0x3fU);
			rgb[index * 3U + 2U] = expand5(pixel & 0x1fU);
		}
		if (fwrite(rgb, (size_t)PREVIEW_WIDTH * PREVIEW_HEIGHT * 3U, 1,
			   stdout) != 1) {
			if (errno != EPIPE)
				fprintf(stderr,
					"fplinux-showcase-preview: write: %s\n",
					strerror(errno));
			armada_scene_destroy(scene);
			free(rgb);
			free(pixels);
			return errno == EPIPE ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}
	armada_scene_destroy(scene);
	free(rgb);
	free(pixels);
	return EXIT_SUCCESS;
}
