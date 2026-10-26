/* SPDX-License-Identifier: GPL-2.0-only */
#include "preparation.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-session.h"

#include <stdio.h>
#include <string.h>

static void rectangle(const struct fplinux_preparation_surface *surface,
		      unsigned int x, unsigned int y, unsigned int width,
		      unsigned int height, uint16_t color)
{
	if (x >= surface->width || y >= surface->height)
		return;
	if (width > surface->width - x)
		width = surface->width - x;
	if (height > surface->height - y)
		height = surface->height - y;
	for (unsigned int row = y; row < y + height; ++row) {
		uint16_t *pixels =
			(uint16_t *)((unsigned char *)surface->pixels +
				     row * surface->stride_bytes);

		for (unsigned int column = x; column < x + width; ++column)
			pixels[column] = color;
	}
}

static void glyph(const struct fplinux_preparation_surface *surface,
		  const struct fplinux_font *font, unsigned int x,
		  unsigned int y, unsigned char character, uint16_t color)
{
	const unsigned char *bitmap = fplinux_font_glyph(font, character);

	if (!bitmap)
		return;
	for (unsigned int row = 0; row < font->height; ++row) {
		for (unsigned int column = 0; column < font->width; ++column) {
			if (bitmap[row * font->row_bytes + column / 8] &
			    (0x80U >> (column % 8)))
				rectangle(surface, x + column, y + row, 1, 1,
					  color);
		}
	}
}

static unsigned int text(const struct fplinux_preparation_surface *surface,
			 const struct fplinux_font *font, unsigned int y,
			 const char *value, uint16_t color)
{
	unsigned int margin = font->width;
	unsigned int columns = surface->width / font->width - 2;
	unsigned int x = margin;

	while (*value) {
		size_t length = strcspn(value, " \n");

		if (x != margin && (x >= margin + columns * font->width ||
				    length > (margin + columns * font->width -
					      x) / font->width)) {
			x = margin;
			y += font->height;
		}
		for (size_t index = 0; index < length; ++index) {
			if (x >= margin + columns * font->width) {
				x = margin;
				y += font->height;
			}
			glyph(surface, font, x, y, (unsigned char)value[index],
			      color);
			x += font->width;
		}
		value += length;
		if (*value == '\n') {
			x = margin;
			y += font->height;
		} else if (*value == ' ' && x != margin) {
			x += font->width;
		}
		if (*value)
			++value;
	}
	return y + font->height;
}

static unsigned int scaled_progress(uint64_t completed, uint64_t total,
				    unsigned int scale)
{
	unsigned int lower = 0;
	unsigned int upper = scale;

	/* Compare against exact thresholds without multiplying a byte count. */
	while (lower < upper) {
		unsigned int middle = lower + (upper - lower + 1) / 2;
		uint64_t threshold =
			total / scale * middle +
			((total % scale) * middle + scale - 1) / scale;

		if (completed >= threshold)
			lower = middle;
		else
			upper = middle - 1;
	}
	return lower;
}

void fplinux_preparation_render(
	const struct fplinux_preparation_surface *surface,
	const struct fplinux_font *font,
	const struct fplinux_preparation_status *status, unsigned int phase)
{
	const struct fplinux_ui_theme_spec *theme =
		fplinux_ui_theme_spec(FPLINUX_UI_SESSION_THEME_DARK);
	uint16_t background =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_BACKGROUND);
	uint16_t foreground =
		fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_FOREGROUND);
	uint16_t muted = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_MUTED);
	uint16_t accent = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_FOCUS);
	uint16_t track = fplinux_ui_colour565(theme, FPLINUX_UI_COLOUR_BORDER);
	const char *heading;
	const char *detail;
	unsigned int margin = font->width;
	unsigned int width = surface->width - 2 * margin;
	unsigned int y = margin;
	unsigned int bar_height = font->height / 2;
	char progress[80];

	rectangle(surface, 0, 0, surface->width, surface->height, background);
	y = text(surface, font, y, "FPLinux", foreground);
	y = text(surface, font, y + 2, "First start", muted);
	y += font->height;
	switch (status->stage) {
	case FPLINUX_PREPARATION_READING:
		heading = "Phone backup";
		detail = "Keep the phone connected.";
		break;
	case FPLINUX_PREPARATION_EXTRACTING:
		heading = "Preparing files";
		detail = "Working on the computer. Please wait.";
		break;
	case FPLINUX_PREPARATION_READY:
		heading = "Ready";
		detail =
			"Power off the phone, then run the same launcher again.";
		break;
	case FPLINUX_PREPARATION_ERROR:
		heading = "Preparation failed";
		detail = "Check the computer log, then try again.";
		break;
	case FPLINUX_PREPARATION_CANCELLED:
		heading = "Cancelled";
		detail = "Check the computer log, then try again.";
		break;
	default:
		heading = "Connecting";
		detail = "Keep the phone connected to the computer.";
		break;
	}
	y = text(surface, font, y, heading, foreground);
	y += font->height / 2;
	if (status->stage == FPLINUX_PREPARATION_READING) {
		unsigned int percent =
			scaled_progress(status->completed, status->total, 100);
		unsigned int filled = scaled_progress(status->completed,
						      status->total, width);

		snprintf(progress, sizeof(progress), "%u%%", percent);
		y = text(surface, font, y, progress, accent);
		rectangle(surface, margin, y + 3, width, bar_height, track);
		rectangle(surface, margin, y + 3, filled, bar_height, accent);
		y += bar_height + font->height;
		snprintf(progress, sizeof(progress), "%.1Lf / %.1Lf MiB",
			 (long double)status->completed / (1024 * 1024),
			 (long double)status->total / (1024 * 1024));
		y = text(surface, font, y, progress, muted);
	} else if (fplinux_preparation_is_busy(status->stage)) {
		unsigned int segment = width / 4;
		unsigned int offset = (phase % 12) * (width - segment) / 11;

		rectangle(surface, margin, y + 3, width, bar_height, track);
		rectangle(surface, margin + offset, y + 3, segment, bar_height,
			  accent);
		y += bar_height + font->height;
	}
	text(surface, font, y + font->height / 2, detail, muted);
}
