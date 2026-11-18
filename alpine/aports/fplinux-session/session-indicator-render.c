/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-indicator-render.h"

#include <stdio.h>
#include <stdlib.h>

#include "fplinux-indicator-masks.h"
#include "fplinux-status-screen.h"
#include "fplinux-ui-profile.h"

#define FPLINUX_INDICATOR_MAX_WIDTH 320
#define FPLINUX_INDICATOR_MAX_HEIGHT 64

static const struct fplinux_indicator_masks *masks_for_size(int size)
{
	for (size_t i = 0; i < sizeof(fplinux_indicator_masks) /
				       sizeof(fplinux_indicator_masks[0]);
	     ++i)
		if (fplinux_indicator_masks[i].size == (unsigned int)size)
			return &fplinux_indicator_masks[i];
	return NULL;
}

static const uint8_t *
level_mask(const struct fplinux_indicator_masks *masks,
	   const struct fplinux_session_indicator *indicator)
{
	switch (indicator->kind) {
	case FPLINUX_SYSTEM_LEVEL_MEDIA:
		return indicator->level.availability ==
					       FPLINUX_SYSTEM_LEVEL_READY &&
				       indicator->level.value ==
					       indicator->level.minimum ?
			       masks->mute :
			       masks->volume;
	case FPLINUX_SYSTEM_LEVEL_ALARM:
		return masks->alarm;
	case FPLINUX_SYSTEM_LEVEL_BRIGHTNESS:
		return masks->brightness;
	default:
		return NULL;
	}
}

static void draw_mask(const struct fplinux_status_canvas *canvas,
		      const uint8_t *alpha, unsigned int size, unsigned int x,
		      unsigned int y, uint16_t foreground, uint16_t background)
{
	for (unsigned int row = 0; row < size; ++row)
		for (unsigned int col = 0; col < size; ++col) {
			unsigned int coverage = alpha[row * size + col];

			if (coverage)
				fplinux_status_fill(
					canvas, x + col, y + row, 1, 1,
					fplinux_status_blend(foreground,
							     background,
							     coverage, 255));
		}
}

void fplinux_session_indicator_frame_clear(
	struct fplinux_session_indicator_frame *frame)
{
	if (!frame)
		return;
	free(frame->pixels);
	*frame = (struct fplinux_session_indicator_frame){ 0 };
}

bool fplinux_session_indicator_render(
	struct fplinux_session_indicator_frame *frame, int screen_width,
	int screen_height, int theme,
	const struct fplinux_session_indicator *indicator)
{
	const struct fplinux_ui_profile_spec *profile;
	const struct fplinux_ui_theme_spec *palette;
	const struct fplinux_status_font *font;
	const struct fplinux_indicator_masks *masks;
	const struct fplinux_system_level_state *level;
	const uint8_t *alpha;
	struct fplinux_status_surface surface;
	struct fplinux_status_canvas canvas;
	struct fplinux_session_indicator_frame next = { 0 };
	uint16_t background, foreground, border, accent;
	unsigned int text_width, bar_width, filled = 0;
	int padding, row_height, text_y, bar_y, bottom;
	char text[16];
	bool ready;

	if (!frame || frame->pixels || !indicator || !indicator->visible ||
	    screen_width <= 0 || screen_width > 65535 || screen_height <= 0 ||
	    screen_height > 65535)
		return false;
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(screen_width, screen_height));
	palette = fplinux_ui_theme_spec(theme);
	if (!profile || !palette)
		return false;
	font = fplinux_status_font_by_id(profile->font_body);
	masks = masks_for_size(profile->row_icon_size);
	if (!font || !font->line_height || !masks)
		return false;
	alpha = level_mask(masks, indicator);
	if (!alpha)
		return false;
	level = &indicator->level;
	ready = level->availability == FPLINUX_SYSTEM_LEVEL_READY &&
		level->minimum < level->maximum && level->maximum <= 65535U &&
		level->value >= level->minimum &&
		level->value <= level->maximum;
	if (ready)
		snprintf(text, sizeof(text), "%u/%u", level->value,
			 level->maximum);
	else
		snprintf(text, sizeof(text), "--");
	text_width = fplinux_status_text_width(font, text);
	padding = profile->margin;
	row_height = font->line_height > masks->size ? font->line_height :
						       (int)masks->size;
	next.width = profile->width - 2 * profile->margin;
	if (next.width > screen_width - 2 * profile->margin)
		next.width = screen_width - 2 * profile->margin;
	next.height = 2 * padding + row_height + profile->spacing +
		      profile->bar_height;
	bottom = screen_height - profile->margin -
		 (indicator->softkeys ? profile->softkey_height : 0);
	if (padding <= 0 || profile->spacing < 0 || profile->bar_height <= 0 ||
	    profile->divider_width <= 0 ||
	    next.width > FPLINUX_INDICATOR_MAX_WIDTH ||
	    next.height > FPLINUX_INDICATOR_MAX_HEIGHT ||
	    next.height > bottom ||
	    next.width < 2 * padding + (int)masks->size + profile->spacing +
				 (int)text_width +
				 (int)fplinux_status_text_width(font, "!"))
		return false;
	next.x = (screen_width - next.width) / 2;
	next.y = bottom - next.height;
	next.stride_bytes = (size_t)next.width * sizeof(uint16_t);
	next.pixels = malloc(next.stride_bytes * (size_t)next.height);
	if (!next.pixels)
		return false;
	surface = (struct fplinux_status_surface){
		.pixels = next.pixels,
		.width = (uint32_t)next.width,
		.height = (uint32_t)next.height,
		.stride_bytes = next.stride_bytes,
	};
	if (fplinux_status_surface_canvas(&canvas, &surface)) {
		fplinux_session_indicator_frame_clear(&next);
		return false;
	}
	background = fplinux_ui_colour565(palette, FPLINUX_UI_COLOUR_POPUP);
	foreground =
		fplinux_ui_colour565(palette, FPLINUX_UI_COLOUR_FOREGROUND);
	border = fplinux_ui_colour565(palette, FPLINUX_UI_COLOUR_BORDER);
	accent = fplinux_ui_colour565(palette, FPLINUX_UI_COLOUR_FOCUS);
	fplinux_status_fill(&canvas, 0, 0, canvas.width, canvas.height, border);
	fplinux_status_fill(
		&canvas, (uint32_t)profile->divider_width,
		(uint32_t)profile->divider_width,
		canvas.width - 2U * (unsigned int)profile->divider_width,
		canvas.height - 2U * (unsigned int)profile->divider_width,
		background);
	draw_mask(&canvas, alpha, masks->size, (unsigned int)padding,
		  (unsigned int)(padding + (row_height - (int)masks->size) / 2),
		  foreground, background);
	text_y = padding + (row_height - font->line_height) / 2;
	fplinux_status_text_draw(&canvas, font,
				 next.width - padding - (int)text_width, text_y,
				 text, foreground, background);
	if (indicator->warning)
		fplinux_status_text_draw(
			&canvas, font,
			next.width - padding - (int)text_width -
				profile->spacing -
				(int)fplinux_status_text_width(font, "!"),
			text_y, "!",
			fplinux_ui_colour565(palette, FPLINUX_UI_COLOUR_DANGER),
			background);
	bar_y = padding + row_height + profile->spacing;
	bar_width = (unsigned int)(next.width - 2 * padding);
	if (ready) {
		fplinux_status_fill(&canvas, (uint32_t)padding, (uint32_t)bar_y,
				    bar_width, (uint32_t)profile->bar_height,
				    border);
		filled = fplinux_status_scale(level->value - level->minimum,
					      level->maximum - level->minimum,
					      bar_width);
		fplinux_status_fill(&canvas, (uint32_t)padding, (uint32_t)bar_y,
				    filled, (uint32_t)profile->bar_height,
				    accent);
	}
	*frame = next;
	return true;
}
