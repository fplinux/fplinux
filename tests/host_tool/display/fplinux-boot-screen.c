/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot-screen.h"
#include "fplinux-boot-mark.h"
#include "generated/fplinux-status-profile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct image {
	uint16_t *pixels;
	uint32_t width, height;
	unsigned int presents;
};

static void fill(void *context, uint32_t x, uint32_t y, uint32_t width,
		 uint32_t height, uint16_t colour)
{
	struct image *image = context;

	assert(x < image->width && width <= image->width - x);
	assert(y < image->height && height <= image->height - y);
	for (uint32_t row = y; row < y + height; ++row)
		for (uint32_t col = x; col < x + width; ++col)
			image->pixels[row * image->width + col] = colour;
}

static void present(void *context)
{
	struct image *image = context;

	++image->presents;
}

static void render_expected_boot(const struct fplinux_status_canvas *canvas)
{
	struct fplinux_status_mark mark = { 0 };
	struct fplinux_status_resources resources = {
		.profile = &fplinux_status_profile,
		.theme = &fplinux_status_themes[1],
		.body = fplinux_status_font_by_id(
			fplinux_status_profile.font_body),
		.title = fplinux_status_font_by_id(
			fplinux_status_profile.font_title),
		.legend = fplinux_status_font_by_id(
			fplinux_status_profile.font_legend),
	};
	const struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_BOOT,
		.indicator = FPLINUX_STATUS_NONE,
		.heading = "Starting FPLinux",
		.detail = "Keep USB connected.",
	};

	for (size_t i = 0; i < FPLINUX_BOOT_MARK_COUNT; ++i) {
		if (fplinux_boot_marks[i].size !=
		    (unsigned int)fplinux_status_profile.splash_mark)
			continue;
		mark.size = fplinux_boot_marks[i].size;
		mark.runs = fplinux_boot_marks[i].runs;
		mark.run_count = fplinux_boot_marks[i].run_count;
		mark.palette = fplinux_boot_mark_palette;
		resources.mark = &mark;
		break;
	}
	assert(resources.mark);
	assert(!fplinux_status_screen_render(canvas, &resources, &view));
}

int main(int argc, char **argv)
{
	struct fplinux_boot_screen screen = { 0 };
	struct fplinux_status_canvas canvas;
	struct image image = { 0 };
	const char *detail;
	FILE *output;

	assert(fplinux_boot_screen_init(NULL, NULL) == -1);
	assert(fplinux_boot_screen_init(&screen, NULL) == -1);
	fplinux_boot_screen_fail(&screen, 1, "x");
	assert(!screen.initialized);
	if (argc < 5 || argc > 6 || sscanf(argv[1], "%u", &image.width) != 1 ||
	    sscanf(argv[2], "%u", &image.height) != 1 || image.width > 320 ||
	    image.height > 320)
		return 2;
	image.pixels =
		calloc((size_t)image.width * image.height, sizeof(uint16_t));
	assert(image.pixels);
	canvas = (struct fplinux_status_canvas){ image.width, image.height,
						 &image, fill, present };
	if (!strcmp(argv[3], "expected"))
		render_expected_boot(&canvas);
	else
		assert(!fplinux_boot_screen_init(&screen, &canvas));
	detail = argc >= 6 ? argv[5] : "Keep USB connected.";
	if (!strcmp(argv[3], "error")) {
		if (argc == 5)
			detail = "Power off and try again.";
		fplinux_boot_screen_fail(&screen, 12, detail);
	} else if (!strcmp(argv[3], "error2")) {
		fplinux_boot_screen_fail(&screen, 13,
					 "Power off and try again.");
	}
	assert(image.presents ==
	       (!strcmp(argv[3], "error") || !strcmp(argv[3], "error2") ? 2U :
									  1U));
	output = fopen(argv[4], "wb");
	assert(output);
	assert(fwrite(image.pixels, sizeof(uint16_t),
		      (size_t)image.width * image.height,
		      output) == (size_t)image.width * image.height);
	assert(!fclose(output));
	free(image.pixels);
	return 0;
}
