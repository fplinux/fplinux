/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-status-screen.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

int main(void)
{
	static const uint8_t bitmap[] = { 0x80, 0xf0 };
	static const struct fplinux_status_glyph glyphs[99] = {
		['A' - ' '] = { 0, 32, 1, 1, 0, 0 },
		['V' - ' '] = { 1, 32, 1, 1, 0, 0 },
	};
	static const struct fplinux_status_kern pairs[] = {
		{ 'A' - ' ', 'U' - ' ', -8 },
		{ 'A' - ' ', 'V' - ' ', -16 },
		{ 'B' - ' ', 'A' - ' ', -8 },
	};
	static const struct fplinux_status_font font = {
		bitmap, glyphs, pairs, 3, 4, 1,
	};
	static const struct fplinux_ui_profile_spec profile = {
		.margin = 1,
		.spacing = 1,
		.bar_height = 1,
	};
	static const struct fplinux_ui_theme_spec theme = {
		.rgb = { [FPLINUX_UI_COLOUR_FOREGROUND] = 0xffffff },
	};
	const struct fplinux_status_resources resources = {
		.profile = &profile,
		.theme = &theme,
		.body = &font,
		.title = &font,
	};
	static const uint16_t wrapped_row[] = {
		0, 0x8c51, 0xffff, 0, 0x8c51, 0xffff, 0, 0,
	};
	struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_PREPARATION, .heading = "AV"
	};
	uint16_t pixels[20 * 24];
	struct fplinux_status_surface surface = { pixels, 20, 24, 40 };
	struct fplinux_status_canvas canvas;

	assert(fplinux_status_mark_top(128, 48) == 27);
	assert(fplinux_status_mark_top(160, 64) == 32);
	assert(fplinux_status_mark_top(220, 80) == 48);
	assert(fplinux_status_mark_top(320, 96) == 80);
	assert(fplinux_status_mark_top(240, 96) == 48);
	assert(fplinux_status_mark_top(1, UINT_MAX) == 0);
	assert(fplinux_status_mark_top(4000000000U, 0) == 1600000000U);
	memset(pixels, 0, sizeof(pixels));
	assert(!fplinux_status_surface_canvas(&canvas, &surface));
	assert(!fplinux_status_screen_render(&canvas, &resources, &view));
	/* The A/V pair advances one pixel; the first glyph has 8/15 alpha. */
	assert(pixels[12 * 20 + 8] == 0x8c51);
	assert(pixels[12 * 20 + 9] == 0xffff);
	assert(pixels[11 * 20 + 8] == 0);
	memset(pixels, 0, sizeof(pixels));
	surface.width = 8;
	view.layout = FPLINUX_STATUS_BOOT;
	view.heading = "AVAVAVAV";
	assert(!fplinux_status_surface_canvas(&canvas, &surface));
	assert(!fplinux_status_screen_render(&canvas, &resources, &view));
	assert(!memcmp(pixels + 10 * 20, wrapped_row, sizeof(wrapped_row)));
	assert(!memcmp(pixels + 14 * 20, wrapped_row, sizeof(wrapped_row)));
	assert(pixels[9 * 20 + 1] == 0);
	assert(pixels[13 * 20 + 1] == 0);
	return 0;
}
