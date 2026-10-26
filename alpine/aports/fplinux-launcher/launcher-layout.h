/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_LAYOUT_H
#define FPLINUX_LAUNCHER_LAYOUT_H

/*
 * The launcher's geometry, computed from the screen profile's tokens
 * and the real surface size without LVGL. Every measurement is in pixels.
 */

#include "fplinux-ui-profile.h"
#include "launcher-power.h"

#include <stdbool.h>
#include <stddef.h>

struct fplinux_launcher_rect {
	int x;
	int y;
	int width;
	int height;
};

/* A rounded rectangle, filled or drawn as a border. */
struct fplinux_launcher_shape {
	struct fplinux_launcher_rect box;
	int radius;
};

/*
 * The launcher surface: the status bar at the top, the soft-key bar at the
 * bottom and the body between them; the catalogue's band at the top of the
 * body and its viewport below the band.
 */
struct fplinux_launcher_frame {
	int width;
	int height;
	int status_height;
	int softkey_height;
	/* Surface coordinates. */
	int body_y;
	int body_height;
	int band_height;
	int viewport_height;
};

void fplinux_launcher_frame(const struct fplinux_ui_profile_spec *spec,
			    int width, int height,
			    struct fplinux_launcher_frame *frame);

/* Offset that centres size in extent; an odd remainder goes after it. */
int fplinux_launcher_centre(int extent, int size);

/*
 * The drawn battery in status-bar coordinates: a 1 px outline with a
 * cap, right-aligned at width - margin and vertically centred; five bars
 * inside; the "!" of the low state; the charging bolt left of it with
 * gap_small. The bolt is a polyline through bolt_points, relative to
 * bolt.x and bolt.y, bolt_line_width wide.
 */
struct fplinux_launcher_battery {
	struct fplinux_launcher_rect body;
	int outline_width;
	int outline_radius;
	struct fplinux_launcher_rect cap;
	struct fplinux_launcher_rect bars[FPLINUX_LAUNCHER_BATTERY_STEP_COUNT];
	struct fplinux_launcher_rect low_stem;
	struct fplinux_launcher_rect low_dot;
	struct fplinux_launcher_rect bolt;
	int bolt_points[4][2];
	int bolt_line_width;
};

void fplinux_launcher_battery_layout(const struct fplinux_ui_profile_spec *spec,
				     int width,
				     struct fplinux_launcher_battery *battery);

/* The lock glyph's square in the status bar's left slot. */
struct fplinux_launcher_rect
fplinux_launcher_status_glyph(const struct fplinux_ui_profile_spec *spec);

/* The grid's columns, at least one. */
size_t fplinux_launcher_columns(const struct fplinux_ui_profile_spec *spec);

/*
 * The catalogue viewport in body coordinates: the rest of the body below the
 * band. Grid rows that do not fit are cut at its
 * edges, the hint that more follows.
 */
struct fplinux_launcher_rect
fplinux_launcher_viewport(const struct fplinux_launcher_frame *frame);

/*
 * The left edge of the tile in column of a grid row: the width the
 * columns tiles of size leave is split into even gaps at both screen edges
 * and between the tiles. Both edge gaps are that share to the nearest pixel,
 * a half rounded down; the gaps between the tiles share the rest and differ
 * by at most a pixel, the larger ones on the right. One column is centred.
 */
int fplinux_launcher_grid_x(int width, int columns, int size, int column);

/*
 * The base gap between grid rows: the gap between the first two columns,
 * or the screen-edge gap with one column. scroll_layout adjusts this only
 * when the viewport would expose a thin strip of the next row.
 */
int fplinux_launcher_grid_gap(int width, int columns, int size);

/*
 * The grid viewport's content: row r starts at padding + r x pitch and is
 * item pixels high; the content ends padding below the last item. Rows of
 * app_icon_size tiles use the adjusted vertical gap from scroll_layout as
 * both their spacing and padding from the band and the soft-key bar.
 */
struct fplinux_launcher_scroll {
	int padding;
	int pitch;
	int item;
	/* Visibility guard around the item, independent of visual spacing. */
	int reveal_margin;
};

void fplinux_launcher_scroll_layout(const struct fplinux_ui_profile_spec *spec,
				    int width, int viewport_height,
				    struct fplinux_launcher_scroll *scroll);

/* The height of rows items' content, 0 without items. */
int fplinux_launcher_content_height(
	const struct fplinux_launcher_scroll *scroll, size_t rows);

/*
 * The scroll offset after the selection moved to row: offset moved by the
 * fewest pixels that show the row with reveal_margin around it, then kept
 * between 0 and the end of rows items' content. The first row returns to 0.
 */
int fplinux_launcher_scroll_offset(const struct fplinux_launcher_scroll *scroll,
				   size_t rows, int viewport_height, int offset,
				   size_t row);

/* The first of rows items wholly below the scroll offset. */
size_t
fplinux_launcher_first_visible_row(const struct fplinux_launcher_scroll *scroll,
				   size_t rows, int offset);

/*
 * The app that digit 1-9 selects: the digit-th app counted in reading order
 * from first_row, the first wholly visible row. count when there is no such
 * app or the digit is outside 1-9.
 */
size_t fplinux_launcher_digit_index(size_t first_row, size_t columns, int digit,
				    size_t count);

enum fplinux_launcher_move {
	FPLINUX_LAUNCHER_MOVE_UP,
	FPLINUX_LAUNCHER_MOVE_DOWN,
	FPLINUX_LAUNCHER_MOVE_LEFT,
	FPLINUX_LAUNCHER_MOVE_RIGHT,
};

/*
 * The selection after a D-pad key among count apps in columns. Up
 * and Down: the same column one row up or down; from a column the short last
 * row lacks, Down selects its last app. Left and Right step in reading order.
 * Moves wrap at the grid's edges.
 */
size_t fplinux_launcher_move(size_t selected, size_t count, size_t columns,
			     enum fplinux_launcher_move move);

/*
 * A grid cell in viewport coordinates: the tile of
 * app_icon_size at fplinux_launcher_grid_x() across and at row's place in
 * scroll down. The selection ring has grid_ring_width and grid_ring_gap
 * between it and the tile; where inset is set, the selected tile shows only
 * its part inside inset pixels of its edge. row counts from the catalogue's
 * first row.
 */
struct fplinux_launcher_cell {
	struct fplinux_launcher_shape tile;
	struct fplinux_launcher_shape ring;
	int inset;
};

void fplinux_launcher_grid_cell(const struct fplinux_ui_profile_spec *spec,
				int width,
				const struct fplinux_launcher_scroll *scroll,
				size_t column, size_t row,
				struct fplinux_launcher_cell *cell);

/*
 * Home and the lock screen: the heights of the clock, the date and
 * the hint, 0 for one not shown, stacked with home_gap between the shown
 * ones and centred in the body. The tops are in body coordinates; a part
 * not shown gets the top of the next one.
 */
struct fplinux_launcher_home {
	int clock_y;
	int date_y;
	int hint_y;
};

void fplinux_launcher_home_layout(const struct fplinux_ui_profile_spec *spec,
				  int body_height, int clock_height,
				  int date_height, int hint_height,
				  struct fplinux_launcher_home *home);

/* Width of a text box centred with margin on both sides. */
int fplinux_launcher_text_width(const struct fplinux_ui_profile_spec *spec,
				int width);

/*
 * A notice: width - 2 x alert_margin
 * wide, at most body_height - 2 x alert_margin high, centred in the body.
 */
struct fplinux_launcher_notice_box {
	int width;
	int max_height;
	int border;
	int padding;
	int gap;
};

void fplinux_launcher_notice_layout(const struct fplinux_ui_profile_spec *spec,
				    int width, int body_height,
				    struct fplinux_launcher_notice_box *box);

/* Launch card in body coordinates; its spinner square belongs to the shell. */
struct fplinux_launcher_launch {
	struct fplinux_launcher_rect icon;
	struct fplinux_launcher_rect spinner;
	struct fplinux_launcher_rect text;
};

void fplinux_launcher_launch_layout(const struct fplinux_ui_profile_spec *spec,
				    const struct fplinux_launcher_frame *frame,
				    struct fplinux_launcher_launch *layout);

/*
 * The icon sizes the launcher looks for under
 * /usr/share/icons/hicolor/<N>x<N>/apps: the row and app icon sizes of every
 * profile plus the supported 48-pixel source size, ascending without repeats.
 * Returns how many there are; at most capacity are written.
 */
size_t fplinux_launcher_icon_sizes(int sizes[], size_t capacity);

#endif
