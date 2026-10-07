/* SPDX-License-Identifier: GPL-2.0-only */
/* Render one production ARMADA frame at the 240x320 panel size. */
#include "armada-scene.h"
#include "screen-frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH 240U
#define SCREEN_HEIGHT 320U

int main(int argc, char **argv)
{
	static uint16_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
	/* Final metrics appear only on the closing card, which is not shown. */
	struct armada_metrics metrics = { .valid = false };
	struct armada_outputs outputs;
	struct armada_scene *scene;
	struct fplinux_font font;
	char *end;
	unsigned long frame;
	bool written;

	if (argc != 4) {
		fprintf(stderr, "usage: %s FRAME FONT.psf OUTPUT\n", argv[0]);
		return EXIT_FAILURE;
	}
	frame = strtoul(argv[1], &end, 10);
	if (*end || frame >= ARMADA_DURATION_FRAMES) {
		fprintf(stderr, "frame must be below %u\n",
			ARMADA_DURATION_FRAMES);
		return EXIT_FAILURE;
	}
	if (!fplinux_font_open(&font, argv[2])) {
		fprintf(stderr, "cannot load font %s\n", argv[2]);
		return EXIT_FAILURE;
	}
	scene = armada_scene_create(SCREEN_WIDTH, SCREEN_HEIGHT, pixels, &font);
	if (!scene) {
		fprintf(stderr, "cannot create scene: %s\n", strerror(errno));
		fplinux_font_close(&font);
		return EXIT_FAILURE;
	}
	armada_scene_render(scene, (uint32_t)frame, &metrics, &outputs);
	armada_scene_destroy(scene);
	fplinux_font_close(&font);
	written = screen_frame_write(argv[3], pixels, SCREEN_WIDTH,
				     SCREEN_HEIGHT,
				     SCREEN_WIDTH * sizeof(pixels[0]));
	return written ? EXIT_SUCCESS : EXIT_FAILURE;
}
