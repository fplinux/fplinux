/* SPDX-License-Identifier: GPL-2.0-only */
#include "preparation.h"
#include "fplinux-activity.h"
#include "fplinux-status-screen.h"
#include "fplinux-ui-session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scenario {
	const char *name;
	struct fplinux_preparation_status status;
	struct fplinux_status_view expected;
};

static const struct scenario scenarios[] = {
	{ "connecting",
	  { FPLINUX_PREPARATION_WAITING, 0, 0 },
	  { .heading = "Connecting",
	    .detail = "Keep USB connected.",
	    .indicator = FPLINUX_STATUS_BUSY } },
	{ "reading-zero",
	  { FPLINUX_PREPARATION_READING, 0, 138412032 },
	  { .heading = "Reading phone data",
	    .detail = "Keep USB connected.",
	    .indicator = FPLINUX_STATUS_PROGRESS,
	    .total = 138412032,
	    .progress_percent = "0%",
	    .progress_bytes = "0.0 / 132.0 MiB" } },
	{ "reading-half",
	  { FPLINUX_PREPARATION_READING, 69206016, 138412032 },
	  { .heading = "Reading phone data",
	    .detail = "Keep USB connected.",
	    .indicator = FPLINUX_STATUS_PROGRESS,
	    .completed = 69206016,
	    .total = 138412032,
	    .progress_percent = "50%",
	    .progress_bytes = "66.0 / 132.0 MiB" } },
	{ "reading-full",
	  { FPLINUX_PREPARATION_READING, 138412032, 138412032 },
	  { .heading = "Reading phone data",
	    .detail = "Keep USB connected.",
	    .indicator = FPLINUX_STATUS_PROGRESS,
	    .completed = 138412032,
	    .total = 138412032,
	    .progress_percent = "100%",
	    .progress_bytes = "132.0 / 132.0 MiB" } },
	{ "extracting",
	  { FPLINUX_PREPARATION_EXTRACTING, 0, 0 },
	  { .heading = "Preparing files",
	    .detail = "Please wait for the computer.",
	    .indicator = FPLINUX_STATUS_BUSY } },
	{ "ready",
	  { FPLINUX_PREPARATION_READY, 0, 0 },
	  { .heading = "Ready",
	    .detail =
		    "Disconnect USB. Power off the phone. Run the launcher again." } },
	{ "cancelled",
	  { FPLINUX_PREPARATION_CANCELLED, 0, 0 },
	  { .heading = "Cancelled",
	    .detail = "Read the message on the computer." } },
	{ "error",
	  { FPLINUX_PREPARATION_ERROR, 0, 0 },
	  { .heading = "Preparation failed",
	    .detail = "Read the message on the computer.",
	    .error = 1 } },
};

int main(int argc, char **argv)
{
	const struct scenario *scenario = NULL;
	struct fplinux_preparation_surface actual;
	struct fplinux_status_surface expected;
	struct fplinux_status_canvas canvas;
	struct fplinux_status_resources resources;
	struct fplinux_status_view view;
	const struct fplinux_ui_profile_spec *profile;
	unsigned int width, height;
	size_t bytes;

	assert(argc == 4);
	assert(sscanf(argv[1], "%u", &width) == 1);
	assert(sscanf(argv[2], "%u", &height) == 1);
	for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
		if (!strcmp(argv[3], scenarios[i].name))
			scenario = &scenarios[i];
	assert(scenario);
	bytes = (size_t)width * height * sizeof(uint16_t);
	actual = (struct fplinux_preparation_surface){
		.pixels = calloc(1, bytes),
		.stride_bytes = width * sizeof(uint16_t),
		.width = width,
		.height = height,
	};
	expected = (struct fplinux_status_surface){
		.pixels = calloc(1, bytes),
		.stride_bytes = width * sizeof(uint16_t),
		.width = width,
		.height = height,
	};
	assert(actual.pixels && expected.pixels);
	assert(fplinux_preparation_render(&actual, &scenario->status, 0));
	assert(!fplinux_status_surface_canvas(&canvas, &expected));
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen((int)width, (int)height));
	assert(profile);
	resources = (struct fplinux_status_resources){
		.profile = profile,
		.theme = fplinux_ui_theme_spec(FPLINUX_UI_SESSION_THEME_DARK),
		.body = fplinux_status_font_by_id(profile->font_body),
		.title = fplinux_status_font_by_id(profile->font_title),
		.legend = fplinux_status_font_by_id(profile->font_legend),
		.activity = fplinux_activity_mask(
			(unsigned int)profile->spinner_size,
			(unsigned int)profile->spinner_width, 0),
	};
	/* Literal views check the adapter; renderer geometry has its own tests. */
	view = scenario->expected;
	view.layout = FPLINUX_STATUS_PREPARATION;
	assert(!fplinux_status_screen_render(&canvas, &resources, &view));
	for (size_t i = 0; i < bytes / sizeof(uint16_t); ++i)
		if (actual.pixels[i] != expected.pixels[i]) {
			fprintf(stderr,
				"%s %ux%u: pixel %zu expected %04x, got %04x\n",
				scenario->name, width, height, i,
				expected.pixels[i], actual.pixels[i]);
			free(expected.pixels);
			free(actual.pixels);
			return 1;
		}
	free(expected.pixels);
	free(actual.pixels);
	return 0;
}
