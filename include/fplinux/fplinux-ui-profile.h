/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The design tokens of every screen profile and theme palette; LVGL-free,
 * not installed and not exported. Compiled into libfplinux-ui and the
 * session so that the runtime, the launcher, the manager and the shell use
 * one table. lib/fplinux/fplinux-ui-profile.c is generated from
 * lib/fplinux/fplinux-ui-profile.json by scripts/fplinux_ui_profile.py.
 * Every measurement is in physical pixels.
 */

#ifndef FPLINUX_UI_PROFILE_H
#define FPLINUX_UI_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Profiles are numbered as enum fplinux_ui_profile of fplinux-ui.h.
 * New profiles append to preserve existing values, independent of geometry.
 */
#define FPLINUX_UI_PROFILE_COUNT 5

struct fplinux_ui_profile_spec {
	int width, height;
	/* Font slots: ids of fonts compiled into the runtime. */
	const char *font_body, *font_title, *font_clock, *font_legend;
	int status_height, softkey_height, header_height;
	int margin, gutter, spacing, gap_small, content_padding, list_padding;
	int scrollbar_width, scrollbar_offset, focus_width, divider_width;
	int row_height, row_height_detail, row_padding, row_gap;
	int row_icon_size, status_icon_size, app_icon_size;
	int grid_columns;
	/* The catalogue opens as a list instead of the grid by default. */
	bool catalogue_list;
	/* Corner radius of a colour tile at row_icon_size and app_icon_size. */
	int row_tile_radius, app_tile_radius;
	/*
	 * Selection rings around a tile, width and gap in pixels, colours as
	 * enum fplinux_ui_colour: in the catalogue grid and on a selected row.
	 * grid_tile_inset: pixels of the selected grid tile's edge painted in
	 * the background, inside the ring's gap.
	 */
	int grid_ring_width, grid_ring_gap, grid_tile_inset, grid_ring_colour;
	int list_tile_ring_width, list_tile_ring_gap, list_tile_ring_colour;
	int frame_padding, field_padding, field_lines, text_box_padding;
	/* The text field status sits right of the text, not below it. */
	bool field_status_inline;
	int picker_columns, picker_cell_width, picker_cell_height,
		picker_border, picker_padding;
	int alert_margin, alert_padding, alert_gap, alert_border;
	int spinner_size, spinner_width, bar_height, menu_edge;
	int legend_margin, legend_gap, home_gap;
	/* The lock screen's unlock hint replaces the date instead of following it. */
	bool home_hint_replaces_date;
	int battery_width, battery_height, battery_cap_width,
		battery_cap_height, battery_bar_width, battery_bar_height,
		bolt_height;
	/* Brand marks are square; About shows a lockup or the mark alone. */
	int splash_mark, about_logo_width;
	bool about_lockup;
};

/* Colour tokens, in the order of struct fplinux_ui_theme. */
enum fplinux_ui_colour {
	FPLINUX_UI_COLOUR_BACKGROUND,
	FPLINUX_UI_COLOUR_SURFACE,
	FPLINUX_UI_COLOUR_FOREGROUND,
	FPLINUX_UI_COLOUR_MUTED,
	FPLINUX_UI_COLOUR_ACCENT,
	FPLINUX_UI_COLOUR_ACCENT_TEXT,
	FPLINUX_UI_COLOUR_BORDER,
	FPLINUX_UI_COLOUR_POPUP,
	FPLINUX_UI_COLOUR_BAR,
	FPLINUX_UI_COLOUR_BAR_TEXT,
	FPLINUX_UI_COLOUR_DISABLED,
	FPLINUX_UI_COLOUR_FOCUS,
	FPLINUX_UI_COLOUR_WARNING,
	FPLINUX_UI_COLOUR_DANGER,
	FPLINUX_UI_COLOUR_TILE_RING,
	FPLINUX_UI_COLOUR_BRAND,
	FPLINUX_UI_COLOUR_SCRIM,
	FPLINUX_UI_COLOUR_COUNT,
};

struct fplinux_ui_theme_spec {
	/* 0xRRGGBB, each exact in RGB565. */
	uint32_t rgb[FPLINUX_UI_COLOUR_COUNT];
	/* Modal backdrop opacity, 0 transparent to 255 opaque. */
	uint8_t scrim_opacity;
};

/*
 * The largest pixel area with width <= width and height <= height; the
 * earlier profile wins equal-area ties. The smallest profile for a screen
 * below all supported sizes.
 */
int fplinux_ui_profile_for_screen(int width, int height);
/* NULL for an unknown profile. */
const struct fplinux_ui_profile_spec *fplinux_ui_profile_spec(int profile);
/*
 * The palette of a theme kind (enum fplinux_ui_theme_kind, the same values
 * as enum fplinux_ui_session_theme); NULL for an unknown kind.
 */
const struct fplinux_ui_theme_spec *fplinux_ui_theme_spec(int kind);

#endif
