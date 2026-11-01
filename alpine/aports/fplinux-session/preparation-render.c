/* SPDX-License-Identifier: GPL-2.0-only */
#include "preparation.h"
#include "fplinux-activity.h"
#include "fplinux-status-screen.h"
#include "fplinux-ui-session.h"

#include <stdio.h>

bool fplinux_preparation_render(
	const struct fplinux_preparation_surface *surface,
	const struct fplinux_preparation_status *status, unsigned int phase)
{
	struct fplinux_status_surface pixels;
	struct fplinux_status_canvas canvas;
	struct fplinux_status_resources resources;
	struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_PREPARATION,
	};
	const struct fplinux_ui_profile_spec *profile;
	char percentage[8], bytes[80];

	if (!surface || !status)
		return false;
	pixels.pixels = surface->pixels;
	pixels.width = surface->width;
	pixels.height = surface->height;
	pixels.stride_bytes = surface->stride_bytes;
	if (fplinux_status_surface_canvas(&canvas, &pixels))
		return false;
	profile = fplinux_ui_profile_spec(fplinux_ui_profile_for_screen(
		(int)surface->width, (int)surface->height));
	if (!profile)
		return false;
	resources.profile = profile;
	resources.theme = fplinux_ui_theme_spec(FPLINUX_UI_SESSION_THEME_DARK);
	resources.body = fplinux_status_font_by_id(profile->font_body);
	resources.title = fplinux_status_font_by_id(profile->font_title);
	resources.legend = fplinux_status_font_by_id(profile->font_legend);
	resources.mark = NULL;
	resources.activity = fplinux_activity_mask(
		(unsigned int)profile->spinner_size,
		(unsigned int)profile->spinner_width, phase);
	switch (status->stage) {
	case FPLINUX_PREPARATION_READING:
		view.heading = "Reading phone data";
		view.detail = "Keep USB connected.";
		if (status->total) {
			view.indicator = FPLINUX_STATUS_PROGRESS;
			view.completed = status->completed;
			view.total = status->total;
			snprintf(percentage, sizeof(percentage), "%u%%",
				 fplinux_status_scale(status->completed,
						      status->total, 100));
			snprintf(bytes, sizeof(bytes), "%.1Lf / %.1Lf MiB",
				 (long double)status->completed / 1048576,
				 (long double)status->total / 1048576);
			view.progress_percent = percentage;
			view.progress_bytes = bytes;
		}
		break;
	case FPLINUX_PREPARATION_EXTRACTING:
		view.heading = "Preparing files";
		view.detail = "Please wait for the computer.";
		view.indicator = FPLINUX_STATUS_BUSY;
		break;
	case FPLINUX_PREPARATION_READY:
		view.heading = "Ready";
		view.detail = "Disconnect USB. Power off the phone. "
			      "Run the launcher again.";
		break;
	case FPLINUX_PREPARATION_ERROR:
		view.heading = "Preparation failed";
		view.detail = "Read the message on the computer.";
		view.error = 1;
		break;
	case FPLINUX_PREPARATION_CANCELLED:
		view.heading = "Cancelled";
		view.detail = "Read the message on the computer.";
		break;
	default:
		view.heading = "Connecting";
		view.detail = "Keep USB connected.";
		view.indicator = FPLINUX_STATUS_BUSY;
		break;
	}
	return fplinux_status_screen_render(&canvas, &resources, &view) == 0;
}
