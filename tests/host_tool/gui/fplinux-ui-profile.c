/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host consumer of layout, theme and screen-selection profile values.
 */
#include "fplinux-ui-profile.h"

#include <stdio.h>
#include <stdlib.h>

static const char *boolean(bool value)
{
	return value ? "true" : "false";
}

static void print_profile(const struct fplinux_ui_profile_spec *spec)
{
	printf("{\"font_body\": \"%s\", \"font_title\": \"%s\", "
	       "\"font_legend\": \"%s\", \"font_clock\": \"%s\", "
	       "\"width\": %d, \"height\": %d, "
	       "\"status_height\": %d, \"softkey_height\": %d, "
	       "\"header_height\": %d, \"margin\": %d, "
	       "\"scrollbar_space\": %d, "
	       "\"row_height\": %d, "
	       "\"app_icon_size\": %d, \"grid_columns\": %d, "
	       "\"row_tile_radius\": %d, \"app_tile_radius\": %d, "
	       "\"grid_ring_width\": %d, \"grid_ring_gap\": %d, "
	       "\"grid_tile_inset\": %d}",
	       spec->font_body, spec->font_title, spec->font_legend,
	       spec->font_clock, spec->width, spec->height, spec->status_height,
	       spec->softkey_height, spec->header_height, spec->margin,
	       spec->scrollbar_space, spec->row_height, spec->app_icon_size,
	       spec->grid_columns, spec->row_tile_radius, spec->app_tile_radius,
	       spec->grid_ring_width, spec->grid_ring_gap,
	       spec->grid_tile_inset);
}

static void print_theme(const struct fplinux_ui_theme_spec *theme)
{
	printf("{\"background\": %lu, \"foreground\": %lu}",
	       (unsigned long)theme->rgb[FPLINUX_UI_COLOUR_BACKGROUND],
	       (unsigned long)theme->rgb[FPLINUX_UI_COLOUR_FOREGROUND]);
}

int main(int argc, char **argv)
{
	printf("{\"profiles\": [");
	for (int i = 0; i < FPLINUX_UI_PROFILE_COUNT; ++i) {
		printf(i ? ", " : "");
		print_profile(fplinux_ui_profile_spec(i));
	}
	printf("], \"themes\": [");
	for (int kind = 0; kind < 2; ++kind) {
		printf(kind ? ", " : "");
		print_theme(fplinux_ui_theme_spec(kind));
	}
	printf("], \"scrim_opacities\": [");
	for (int kind = 0; kind < 2; ++kind)
		printf("%s%u", kind ? ", " : "",
		       (unsigned int)fplinux_ui_theme_spec(kind)->scrim_opacity);
	printf("], \"unknown\": [%s, %s, %s, %s]",
	       boolean(!fplinux_ui_profile_spec(-1)),
	       boolean(!fplinux_ui_profile_spec(FPLINUX_UI_PROFILE_COUNT)),
	       boolean(!fplinux_ui_theme_spec(-1)),
	       boolean(!fplinux_ui_theme_spec(2)));
	printf(", \"selection\": {");
	for (int i = 1; i < argc; ++i) {
		int width;
		int height;

		if (sscanf(argv[i], "%dx%d", &width, &height) != 2)
			return 2;
		printf("%s\"%s\": %d", i > 1 ? ", " : "", argv[i],
		       fplinux_ui_profile_for_screen(width, height));
	}
	printf("}}\n");
	return EXIT_SUCCESS;
}
