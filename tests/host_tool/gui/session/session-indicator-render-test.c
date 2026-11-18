/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-indicator-render.h"
#include "fplinux-status-screen.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *select_scenario(const char *name,
				   struct fplinux_session_indicator *indicator)
{
	const char *number = "7/10";

	*indicator = (struct fplinux_session_indicator){
		.visible = true,
		.kind = FPLINUX_SYSTEM_LEVEL_MEDIA,
		.level = {
			.availability = FPLINUX_SYSTEM_LEVEL_READY,
			.maximum = 10,
			.value = 7,
		},
	};
	if (!strcmp(name, "pending")) {
		indicator->level.pending = true;
	} else if (!strcmp(name, "zero")) {
		indicator->level.value = 0;
		number = "0/10";
	} else if (!strcmp(name, "full")) {
		indicator->level.value = 10;
		number = "10/10";
	} else if (!strcmp(name, "alarm")) {
		indicator->kind = FPLINUX_SYSTEM_LEVEL_ALARM;
		indicator->level.value = 3;
		number = "3/10";
	} else if (!strcmp(name, "brightness")) {
		indicator->kind = FPLINUX_SYSTEM_LEVEL_BRIGHTNESS;
		indicator->level.minimum = 1;
		indicator->level.maximum = 5;
		indicator->level.value = 4;
		number = "4/5";
	} else if (!strcmp(name, "unknown") || !strcmp(name, "unavailable")) {
		indicator->level.availability =
			!strcmp(name, "unknown") ?
				FPLINUX_SYSTEM_LEVEL_UNKNOWN :
				FPLINUX_SYSTEM_LEVEL_UNAVAILABLE;
		indicator->level.value = 0;
		number = "--";
	} else if (!strcmp(name, "warning")) {
		indicator->warning = true;
	} else {
		assert(!strcmp(name, "ready"));
	}
	return number;
}

static void check_number(const struct fplinux_session_indicator_frame *frame,
			 const struct fplinux_ui_profile_spec *profile,
			 const struct fplinux_ui_theme_spec *theme,
			 const char *number)
{
	const struct fplinux_status_font *font =
		fplinux_status_font_by_id(profile->font_body);
	struct fplinux_status_surface surface = {
		.width = (uint32_t)frame->width,
		.height = (uint32_t)frame->height,
		.stride_bytes = frame->stride_bytes,
	};
	struct fplinux_status_canvas canvas;
	unsigned int width = fplinux_status_text_width(font, number);
	int x = frame->width - profile->margin - (int)width;
	int row_height = profile->row_icon_size > font->line_height ?
				 profile->row_icon_size :
				 font->line_height;
	int y = profile->margin + (row_height - font->line_height) / 2;
	uint16_t background =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_POPUP);
	bool has_ink = false;

	surface.pixels = calloc((size_t)frame->height, frame->stride_bytes);
	assert(surface.pixels);
	assert(!fplinux_status_surface_canvas(&canvas, &surface));
	fplinux_status_fill(&canvas, 0, 0, canvas.width, canvas.height,
			    background);
	fplinux_status_text_draw(
		&canvas, font, x, y, number,
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_FOREGROUND),
		background);
	for (int row = y; row < y + font->line_height; ++row)
		for (int col = x; col < x + (int)width; ++col) {
			size_t offset = (size_t)row * (size_t)frame->width +
					(size_t)col;

			assert(frame->pixels[offset] == surface.pixels[offset]);
			has_ink |= surface.pixels[offset] != background;
		}
	assert(has_ink);
	free(surface.pixels);
}

static void
check_frame_lifetime(int width, int height, int theme,
		     const struct fplinux_session_indicator *indicator,
		     struct fplinux_session_indicator_frame *original)
{
	struct fplinux_session_indicator changed = *indicator;
	struct fplinux_session_indicator_frame next = { 0 };
	struct fplinux_session_indicator_frame rejected = { 0 };
	size_t bytes = original->stride_bytes * (size_t)original->height;
	uint16_t *snapshot = malloc(bytes);

	assert(snapshot);
	memcpy(snapshot, original->pixels, bytes);
	changed.warning = !changed.warning;
	assert(fplinux_session_indicator_render(&next, width, height, theme,
						&changed));
	assert(next.pixels != original->pixels);
	assert(!memcmp(snapshot, original->pixels, bytes));
	assert(!fplinux_session_indicator_render(original, width, height, theme,
						 &changed));
	assert(!memcmp(snapshot, original->pixels, bytes));
	fplinux_session_indicator_frame_clear(&next);
	assert(!next.pixels && !next.width && !next.height &&
	       !next.stride_bytes);
	assert(!memcmp(snapshot, original->pixels, bytes));
	changed.visible = false;
	assert(!fplinux_session_indicator_render(&rejected, width, height,
						 theme, &changed));
	assert(!rejected.pixels);
	changed.visible = true;
	assert(!fplinux_session_indicator_render(&rejected, 1, 1, theme,
						 &changed));
	assert(!rejected.pixels);
	assert(!fplinux_session_indicator_render(&rejected, width, height, 99,
						 &changed));
	assert(!rejected.pixels);
	free(snapshot);
}

int main(int argc, char **argv)
{
	struct fplinux_session_indicator indicator;
	struct fplinux_session_indicator_frame frame = { 0 };
	const struct fplinux_ui_profile_spec *profile;
	const char *number;
	FILE *output;
	int width, height, theme;

	assert(argc == 8);
	assert(sscanf(argv[1], "%d", &width) == 1);
	assert(sscanf(argv[2], "%d", &height) == 1);
	assert(width > 0 && height > 0);
	theme = !strcmp(argv[3], "dark") ? 1 : 0;
	number = select_scenario(argv[4], &indicator);
	indicator.softkeys = !strcmp(argv[5], "softkeys");
	assert(fplinux_session_indicator_render(&frame, width, height, theme,
						&indicator));
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	assert(profile);
	check_number(&frame, profile, fplinux_ui_theme_spec(theme), number);
	check_frame_lifetime(width, height, theme, &indicator, &frame);
	output = fopen(argv[6], "wb");
	assert(output);
	assert(fwrite(frame.pixels, frame.stride_bytes, (size_t)frame.height,
		      output) == (size_t)frame.height);
	assert(!fclose(output));
	printf("{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}\n", frame.x,
	       frame.y, frame.width, frame.height);
	if (!strcmp(argv[7], "preview")) {
		uint16_t background =
			fplinux_ui_colour565(fplinux_ui_theme_spec(theme),
					     FPLINUX_UI_COLOUR_BACKGROUND);
		uint16_t *screen = malloc((size_t)width * (size_t)height *
					  sizeof(*screen));
		char path[4096];

		assert(screen);
		for (size_t i = 0; i < (size_t)width * (size_t)height; ++i)
			screen[i] = background;
		for (int row = 0; row < frame.height; ++row)
			memcpy(screen +
				       (size_t)(frame.y + row) * (size_t)width +
				       (size_t)frame.x,
			       frame.pixels + (size_t)row * (size_t)frame.width,
			       frame.stride_bytes);
		assert(snprintf(path, sizeof(path), "%s-screen.rgb565",
				argv[6]) > 0);
		output = fopen(path, "wb");
		assert(output);
		assert(fwrite(screen, sizeof(*screen),
			      (size_t)width * (size_t)height,
			      output) == (size_t)width * (size_t)height);
		assert(!fclose(output));
		free(screen);
	}
	fplinux_session_indicator_frame_clear(&frame);
	return 0;
}
