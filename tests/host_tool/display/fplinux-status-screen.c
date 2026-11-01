/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-activity.h"
#include "fplinux-status-screen.h"
#include "fplinux-ui-session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct fplinux_status_surface surface;
	struct fplinux_status_canvas canvas;
	struct fplinux_status_resources resources;
	struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_PREPARATION,
		.heading = "Reading phone data",
		.detail = "Keep USB connected.",
		.progress_percent = "50%",
		.progress_bytes = "66.0 / 132.0 MiB",
		.total = UINT64_MAX,
	};
	const struct fplinux_ui_profile_spec *profile;
	unsigned int width, height, phase;
	FILE *output;
	uint16_t *pixels;
	size_t stride;

	if (argc != 7 || sscanf(argv[1], "%u", &width) != 1 ||
	    sscanf(argv[2], "%u", &height) != 1 ||
	    sscanf(argv[5], "%u", &phase) != 1 || width > 320 || height > 320)
		return 2;
	stride = width + 4U;
	pixels = malloc((stride * height + 8U) * sizeof(*pixels));
	assert(pixels);
	for (size_t i = 0; i < stride * height + 8U; ++i)
		pixels[i] = 0xa55a;
	surface = (struct fplinux_status_surface){ pixels, width, height,
						   stride * sizeof(*pixels) };
	assert(!fplinux_status_surface_canvas(&canvas, &surface));
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	assert(profile);
	resources = (struct fplinux_status_resources){
		.profile = profile,
		.theme = fplinux_ui_theme_spec(
			!strcmp(argv[3], "dark") ?
				FPLINUX_UI_SESSION_THEME_DARK :
				FPLINUX_UI_SESSION_THEME_LIGHT),
		.body = fplinux_status_font_by_id(profile->font_body),
		.title = fplinux_status_font_by_id(profile->font_title),
		.legend = fplinux_status_font_by_id(profile->font_legend),
		.activity = fplinux_activity_mask(
			profile->spinner_size, profile->spinner_width, phase),
	};
	if (!strcmp(argv[4], "busy")) {
		view.indicator = FPLINUX_STATUS_BUSY;
		view.heading = "Connecting";
	} else if (!strcmp(argv[4], "extracting")) {
		view.indicator = FPLINUX_STATUS_BUSY;
		view.heading = "Preparing files";
		view.detail = "Computer processing. Please wait.";
	} else if (!strcmp(argv[4], "ready")) {
		view.heading = "Ready";
		view.detail =
			"Disconnect USB. Power off the phone, then run the launcher again.";
	} else if (!strcmp(argv[4], "cancelled")) {
		view.heading = "Cancelled";
		view.detail = "Check the computer for next steps.";
	} else if (!strcmp(argv[4], "error")) {
		view.error = 1;
		view.heading = "Preparation failed";
		view.detail = "Check the computer for the error.";
	} else {
		view.indicator = FPLINUX_STATUS_PROGRESS;
		view.completed = !strcmp(argv[4], "zero") ? 0 :
				 !strcmp(argv[4], "full") ? UINT64_MAX :
							    UINT64_MAX / 2;
		if (!strcmp(argv[4], "zero")) {
			view.progress_percent = "0%";
			view.progress_bytes = "0.0 / 132.0 MiB";
		} else if (!strcmp(argv[4], "full")) {
			view.progress_percent = "100%";
			view.progress_bytes = "132.0 / 132.0 MiB";
		}
	}
	assert(!fplinux_status_screen_render(&canvas, &resources, &view));
	for (unsigned int row = 0; row < height; ++row)
		for (unsigned int col = width; col < stride; ++col)
			assert(pixels[row * stride + col] == 0xa55a);
	for (size_t i = stride * height; i < stride * height + 8U; ++i)
		assert(pixels[i] == 0xa55a);
	output = fopen(argv[6], "wb");
	assert(output);
	for (unsigned int row = 0; row < height; ++row)
		assert(fwrite(pixels + row * stride, sizeof(*pixels), width,
			      output) == width);
	assert(!fclose(output));
	free(pixels);
	return 0;
}
