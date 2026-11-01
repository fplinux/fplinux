/* SPDX-License-Identifier: GPL-2.0-only */
#include "brightness-ui.h"
#include "fplinux-keypad.h"
#include "fplinux-status-screen.h"
#include "fplinux-ui-session.h"

enum brightness_ui_action brightness_ui_key(unsigned int level,
					    unsigned int code,
					    unsigned int *requested)
{
	if (code == FPLINUX_KEY_SOFT_RIGHT)
		return BRIGHTNESS_UI_EXIT;
	if (!requested || level > 10U)
		return BRIGHTNESS_UI_NONE;
	if (code == FPLINUX_KEY_UP && level < 10U) {
		*requested = level + 1U;
		return BRIGHTNESS_UI_SET;
	}
	if (code == FPLINUX_KEY_DOWN && level > 0U) {
		*requested = level - 1U;
		return BRIGHTNESS_UI_SET;
	}
	return BRIGHTNESS_UI_NONE;
}

bool brightness_ui_render(const struct brightness_ui_surface *surface,
			  unsigned int level, bool set_failed)
{
	struct fplinux_status_surface pixels;
	struct fplinux_status_canvas canvas;
	struct fplinux_status_resources resources;
	struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_BRIGHTNESS,
		.header = "Brightness",
		.detail = set_failed ? "Could not set brightness." :
				       "Up / Down: adjust",
		.action = "Right soft: Exit",
		.level = level,
		.error = set_failed,
	};
	const struct fplinux_ui_profile_spec *profile;
	char value[] = "00/10";

	if (!surface || level > 10U)
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
	resources.activity = NULL;
	value[0] = (char)('0' + level / 10U);
	value[1] = (char)('0' + level % 10U);
	view.heading = level < 10U ? value + 1 : value;
	return fplinux_status_screen_render(&canvas, &resources, &view) == 0;
}
