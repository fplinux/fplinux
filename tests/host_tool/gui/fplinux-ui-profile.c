/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host harness of the UI profile table: prints every profile, every palette,
 * the palette of each theme kind and the profile chosen for each WxH screen
 * given as an argument, as one JSON object.
 */
#include "fplinux-ui-profile.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FIELD(name) { #name, offsetof(struct fplinux_ui_profile_spec, name) }

struct field {
	const char *name;
	size_t offset;
};

static const struct field int_fields[] = {
	FIELD(width),
	FIELD(height),
	FIELD(status_height),
	FIELD(softkey_height),
	FIELD(header_height),
	FIELD(margin),
	FIELD(gutter),
	FIELD(spacing),
	FIELD(gap_small),
	FIELD(content_padding),
	FIELD(list_padding),
	FIELD(scrollbar_width),
	FIELD(scrollbar_offset),
	FIELD(focus_width),
	FIELD(divider_width),
	FIELD(row_height),
	FIELD(row_height_detail),
	FIELD(row_padding),
	FIELD(row_gap),
	FIELD(row_icon_size),
	FIELD(status_icon_size),
	FIELD(app_icon_size),
	FIELD(grid_columns),
	FIELD(grid_scrollbar_offset),
	FIELD(row_tile_radius),
	FIELD(app_tile_radius),
	FIELD(grid_ring_width),
	FIELD(grid_ring_gap),
	FIELD(grid_tile_inset),
	FIELD(grid_ring_colour),
	FIELD(list_tile_ring_width),
	FIELD(list_tile_ring_gap),
	FIELD(list_tile_ring_colour),
	FIELD(frame_padding),
	FIELD(field_padding),
	FIELD(field_lines),
	FIELD(text_box_padding),
	FIELD(picker_columns),
	FIELD(picker_cell_width),
	FIELD(picker_cell_height),
	FIELD(picker_border),
	FIELD(picker_padding),
	FIELD(alert_margin),
	FIELD(alert_padding),
	FIELD(alert_gap),
	FIELD(alert_border),
	FIELD(spinner_size),
	FIELD(spinner_width),
	FIELD(bar_height),
	FIELD(menu_edge),
	FIELD(legend_margin),
	FIELD(legend_gap),
	FIELD(home_gap),
	FIELD(battery_width),
	FIELD(battery_height),
	FIELD(battery_cap_width),
	FIELD(battery_cap_height),
	FIELD(battery_bar_width),
	FIELD(battery_bar_height),
	FIELD(bolt_height),
	FIELD(splash_mark),
	FIELD(boot_mark),
	FIELD(about_logo_width),
};

static const char *const colour_names[FPLINUX_UI_COLOUR_COUNT] = {
	[FPLINUX_UI_COLOUR_BACKGROUND] = "background",
	[FPLINUX_UI_COLOUR_SURFACE] = "surface",
	[FPLINUX_UI_COLOUR_FOREGROUND] = "foreground",
	[FPLINUX_UI_COLOUR_MUTED] = "muted",
	[FPLINUX_UI_COLOUR_ACCENT] = "accent",
	[FPLINUX_UI_COLOUR_ACCENT_TEXT] = "accent_text",
	[FPLINUX_UI_COLOUR_BORDER] = "border",
	[FPLINUX_UI_COLOUR_POPUP] = "popup",
	[FPLINUX_UI_COLOUR_BAR] = "bar",
	[FPLINUX_UI_COLOUR_BAR_TEXT] = "bar_text",
	[FPLINUX_UI_COLOUR_DISABLED] = "disabled",
	[FPLINUX_UI_COLOUR_FOCUS] = "focus",
	[FPLINUX_UI_COLOUR_WARNING] = "warning",
	[FPLINUX_UI_COLOUR_DANGER] = "danger",
	[FPLINUX_UI_COLOUR_TILE_RING] = "tile_ring",
	[FPLINUX_UI_COLOUR_BRAND] = "brand",
	[FPLINUX_UI_COLOUR_SCRIM] = "scrim",
};

static const char *boolean(bool value)
{
	return value ? "true" : "false";
}

static void print_profile(const struct fplinux_ui_profile_spec *spec)
{
	printf("{\"font_body\": \"%s\", \"font_title\": \"%s\", "
	       "\"font_clock\": \"%s\", \"font_legend\": \"%s\", "
	       "\"catalogue_list\": %s, "
	       "\"field_status_inline\": %s, \"about_lockup\": %s, "
	       "\"home_hint_replaces_date\": %s",
	       spec->font_body, spec->font_title, spec->font_clock,
	       spec->font_legend, boolean(spec->catalogue_list),
	       boolean(spec->field_status_inline), boolean(spec->about_lockup),
	       boolean(spec->home_hint_replaces_date));
	for (size_t i = 0; i < ARRAY_SIZE(int_fields); ++i)
		printf(", \"%s\": %d", int_fields[i].name,
		       *(const int *)((const char *)spec +
				      int_fields[i].offset));
	printf("}");
}

static void print_palette(const struct fplinux_ui_theme_spec *palette)
{
	printf("{\"kind\": %d", palette->kind);
	for (int i = 0; i < FPLINUX_UI_COLOUR_COUNT; ++i)
		printf(", \"%s\": %lu", colour_names[i],
		       (unsigned long)palette->rgb[i]);
	printf("}");
}

int main(int argc, char **argv)
{
	printf("{\"profiles\": [");
	for (int i = 0; i < FPLINUX_UI_PROFILE_COUNT; ++i) {
		printf(i ? ", " : "");
		print_profile(fplinux_ui_profile_spec(i));
	}
	printf("], \"palettes\": [");
	for (int i = 0; i < FPLINUX_UI_PALETTE_COUNT; ++i) {
		printf(i ? ", " : "");
		print_palette(fplinux_ui_palette_spec(i));
	}
	printf("], \"themes\": [");
	for (int kind = 0; kind < 2; ++kind) {
		printf(kind ? ", " : "");
		print_palette(fplinux_ui_theme_spec(kind));
	}
	printf("], \"scrim_opacities\": [");
	for (int kind = 0; kind < 2; ++kind)
		printf("%s%u", kind ? ", " : "",
		       (unsigned int)fplinux_ui_theme_spec(kind)->scrim_opacity);
	printf("], \"unknown\": [%s, %s, %s, %s, %s, %s]",
	       boolean(!fplinux_ui_profile_spec(-1)),
	       boolean(!fplinux_ui_profile_spec(FPLINUX_UI_PROFILE_COUNT)),
	       boolean(!fplinux_ui_palette_spec(-1)),
	       boolean(!fplinux_ui_palette_spec(FPLINUX_UI_PALETTE_COUNT)),
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
