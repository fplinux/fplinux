/* SPDX-License-Identifier: GPL-2.0-only */
/* Render one production ARMADA frame at the 240x320 panel size. */
#include "armada-scene.h"
#include "screen-frame.h"

#include <stdio.h>
#include <stdlib.h>

#define SCREEN_WIDTH 240U
#define SCREEN_HEIGHT 320U

int main(int argc, char **argv)
{
	struct armada_outputs outputs;
	struct armada_scene *scene;
	char error[512];
	char *end;
	unsigned long frame;
	bool written;

	if (argc != 3) {
		fprintf(stderr, "usage: %s FRAME OUTPUT\n", argv[0]);
		return EXIT_FAILURE;
	}
	frame = strtoul(argv[1], &end, 10);
	if (*end || frame >= ARMADA_DURATION_FRAMES) {
		fprintf(stderr, "frame must be below %u\n",
			ARMADA_DURATION_FRAMES);
		return EXIT_FAILURE;
	}
	scene = armada_scene_create_with_renderer(SCREEN_WIDTH, SCREEN_HEIGHT,
						  ARMADA_RENDER_SOFTWARE, error,
						  sizeof(error));
	if (!scene) {
		fprintf(stderr, "cannot create scene: %s\n", error);
		return EXIT_FAILURE;
	}
	if (!armada_scene_render(scene, (uint32_t)frame, &outputs)) {
		fprintf(stderr, "cannot render scene: %s\n",
			armada_scene_error(scene));
		armada_scene_destroy(scene);
		return EXIT_FAILURE;
	}
	written = screen_frame_write(argv[2], armada_scene_pixels(scene),
				     SCREEN_WIDTH, SCREEN_HEIGHT,
				     SCREEN_WIDTH * sizeof(uint16_t));
	armada_scene_destroy(scene);
	return written ? EXIT_SUCCESS : EXIT_FAILURE;
}
