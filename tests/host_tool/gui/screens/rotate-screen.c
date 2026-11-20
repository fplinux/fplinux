/* SPDX-License-Identifier: GPL-2.0-only */
/* Rotate an RGB565 frame with the production CPU rotation engine. */
#include "fplinux-rotate.h"
#include "screen-frame.h"

#include <stdio.h>
#include <stdlib.h>

static bool parse(const char *text, uint32_t *value)
{
	char *end;
	unsigned long number = strtoul(text, &end, 10);

	*value = (uint32_t)number;
	return *text && !*end && number <= 4096U;
}

int main(int argc, char **argv)
{
	struct fplinux_rotate_transform transform = { 0 };
	struct fplinux_rotate_image source = {
		.format = FPLINUX_ROTATE_RGB565,
		.planes = 1U,
	};
	struct fplinux_rotate_image destination = source;
	uint16_t *input;
	uint16_t *output;
	bool done = false;

	if (argc != 6 || !parse(argv[2], &source.width) ||
	    !parse(argv[3], &source.height) ||
	    !parse(argv[4], &transform.rotation)) {
		fprintf(stderr, "usage: %s INPUT WIDTH HEIGHT DEGREES OUTPUT\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	transform.width = source.width;
	transform.height = source.height;
	if (!fplinux_rotate_dimensions(&transform, &destination.width,
				       &destination.height)) {
		fprintf(stderr, "unsupported rotation %s\n", argv[4]);
		return EXIT_FAILURE;
	}
	input = calloc((size_t)source.width * source.height, sizeof(*input));
	output = calloc((size_t)source.width * source.height, sizeof(*output));
	if (!input || !output)
		goto cleanup;
	source.plane[0] = (struct fplinux_rotate_plane){
		.data = (uint8_t *)input,
		.size = (size_t)source.width * source.height * sizeof(*input),
		.stride = source.width * sizeof(*input),
	};
	destination.plane[0] = (struct fplinux_rotate_plane){
		.data = (uint8_t *)output,
		.size = (size_t)destination.width * destination.height *
			sizeof(*output),
		.stride = destination.width * sizeof(*output),
	};
	if (!screen_frame_read(argv[1], input, source.width, source.height))
		goto cleanup;
	if (!fplinux_rotate_cpu(&source, &destination, &transform)) {
		fprintf(stderr, "CPU rotation rejected the frame\n");
		goto cleanup;
	}
	done = screen_frame_write(argv[5], output, destination.width,
				  destination.height,
				  destination.plane[0].stride);
cleanup:
	free(output);
	free(input);
	return done ? EXIT_SUCCESS : EXIT_FAILURE;
}
