/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-status-profile-generated.h"

#include <assert.h>
#include <string.h>

int main(void)
{
	const struct fplinux_ui_profile_spec *profile =
		fplinux_ui_profile_spec(PROFILE_INDEX);

	assert(profile);
	assert(profile->width == fplinux_status_profile.width);
	assert(profile->height == fplinux_status_profile.height);
	assert(profile->header_height == fplinux_status_profile.header_height);
	assert(profile->softkey_height ==
	       fplinux_status_profile.softkey_height);
	assert(profile->margin == fplinux_status_profile.margin);
	assert(profile->spacing == fplinux_status_profile.spacing);
	assert(profile->spinner_size == fplinux_status_profile.spinner_size);
	assert(profile->bar_height == fplinux_status_profile.bar_height);
	assert(!strcmp(profile->font_body, fplinux_status_profile.font_body));
	assert(!strcmp(profile->font_title, fplinux_status_profile.font_title));
	assert(!strcmp(profile->font_legend,
		       fplinux_status_profile.font_legend));
	for (int i = 0; i < 2; ++i) {
		const struct fplinux_ui_theme_spec *theme =
			fplinux_ui_theme_spec(i);

		assert(theme);
		assert(theme->scrim_opacity ==
		       fplinux_status_themes[i].scrim_opacity);
		for (int colour = 0; colour < FPLINUX_UI_COLOUR_COUNT; ++colour)
			assert(theme->rgb[colour] ==
			       fplinux_status_themes[i].rgb[colour]);
	}
	return 0;
}
