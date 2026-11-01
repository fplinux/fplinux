/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-catalogue.h"
#include "launcher-layout.h"
#include "launcher-power.h"
#include "launcher-text.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int failures;

static void check(bool condition, const char *description)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", description);
		++failures;
	}
}

static void test_text(void)
{
	static const struct {
		int percent;
		int bars;
	} battery[] = {
		{ -1, 0 }, { 0, 0 },  { 14, 0 },  { 15, 1 }, { 34, 1 },
		{ 35, 2 }, { 54, 2 }, { 55, 3 },  { 74, 3 }, { 75, 4 },
		{ 94, 4 }, { 95, 5 }, { 100, 5 },
	};
	struct timespec now = { .tv_sec = 119, .tv_nsec = 250000001 };
	enum fplinux_launcher_view view = FPLINUX_LAUNCHER_VIEW_HOME;
	enum fplinux_launcher_view presentation = FPLINUX_LAUNCHER_VIEW_LIST;
	char text[64];

	for (size_t i = 0; i < sizeof(battery) / sizeof(*battery); ++i)
		check(fplinux_launcher_battery_steps(battery[i].percent) ==
			      battery[i].bars,
		      "battery bar thresholds");
	check(fplinux_launcher_notice_from_code("battery-critical") ==
		      FPLINUX_LAUNCHER_NOTICE_BATTERY_CRITICAL,
	      "critical battery notice is distinct");
	check(!strcmp(fplinux_launcher_notice_code(
			      FPLINUX_LAUNCHER_NOTICE_BATTERY_LOW),
		      "battery-low"),
	      "dismiss uses the manager's battery code");
	check(fplinux_launcher_notice_is_unlock_hint(
		      fplinux_launcher_notice_from_code("unlock-press-star")),
	      "second unlock step is a hint");
	check(!strcmp(fplinux_launcher_notice_body(
			      FPLINUX_LAUNCHER_NOTICE_UNLOCK_HELP),
		      "Press Unlock, then *"),
	      "lock help retains the two-step instruction");
	check(fplinux_launcher_notice_from_code("unknown-action") ==
		      FPLINUX_LAUNCHER_NOTICE_FAILED,
	      "unknown nonempty notices remain observable failures");
	check(fplinux_launcher_ms_to_next_minute(&now) == 750,
	      "minute tick rounds the sub-millisecond delay upward");
	now = (struct timespec){ .tv_sec = 120, .tv_nsec = 0 };
	check(fplinux_launcher_ms_to_next_minute(&now) == 60000,
	      "exact minute schedules the following minute");
	check(!fplinux_launcher_clock_is_ready(0) &&
		      !fplinux_launcher_clock_is_ready(315532799),
	      "the startup epoch and dates before 1980 remain unknown");
	check(fplinux_launcher_clock_is_ready(315532800) &&
		      fplinux_launcher_clock_is_ready(1792373001),
	      "1980 and the synchronized startup date are supported clocks");
	check(fplinux_launcher_clock_is_ready((time_t)4102444799LL) &&
		      !fplinux_launcher_clock_is_ready((time_t)4102444800LL),
	      "the supported date range ends with 2099");
	now = (struct timespec){ .tv_sec = 0, .tv_nsec = 0 };
	check(fplinux_launcher_clock_delay(&now) == 1000,
	      "unknown startup time retries without waiting an epoch minute");
	now = (struct timespec){ .tv_sec = 1792373001, .tv_nsec = 250000001 };
	check(fplinux_launcher_clock_delay(&now) == 38750,
	      "after synchronization the timer returns to the next minute");
	now = (struct timespec){ .tv_sec = 315532800, .tv_nsec = 0 };
	check(fplinux_launcher_clock_delay(&now) == 60000,
	      "a valid exact minute does not retain one-second polling");
	check(fplinux_launcher_view_from_names("catalogue", "grid", &view,
					       &presentation) &&
		      view == FPLINUX_LAUNCHER_VIEW_GRID &&
		      presentation == FPLINUX_LAUNCHER_VIEW_GRID,
	      "restarted launcher restores the catalogue presentation");
	check(fplinux_launcher_view_from_names("home", "list", &view,
					       &presentation) &&
		      view == FPLINUX_LAUNCHER_VIEW_HOME &&
		      presentation == FPLINUX_LAUNCHER_VIEW_LIST,
	      "Home remembers the list presentation");
	check(!fplinux_launcher_view_from_names("unknown", "grid", &view,
						&presentation) &&
		      view == FPLINUX_LAUNCHER_VIEW_HOME &&
		      presentation == FPLINUX_LAUNCHER_VIEW_LIST,
	      "invalid manager view leaves both choices unchanged");
	check(fplinux_launcher_split_date("Sunday, 27 September", text,
					  sizeof(text)) &&
		      !strcmp(text, "Sunday,\n27 September"),
	      "long dates break after the weekday");
	strcpy(text, "unchanged");
	check(!fplinux_launcher_split_date("27/09/2026", text, sizeof(text)) &&
		      !strcmp(text, "unchanged"),
	      "a short date is not split");
}

static void test_navigation(void)
{
	struct fplinux_launcher_scroll scroll = {
		.padding = 4,
		.pitch = 20,
		.item = 16,
		.reveal_margin = 2,
	};

	check(fplinux_launcher_move(0, 10, 3, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "grid Up wraps from the first row into the last row");
	check(fplinux_launcher_move(2, 10, 3, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "grid Up clamps a missing last-row column to the final app");
	check(fplinux_launcher_move(1, 11, 3, FPLINUX_LAUNCHER_MOVE_UP) == 10,
	      "grid Up preserves the column when it exists in the last row");
	check(fplinux_launcher_move(9, 11, 3, FPLINUX_LAUNCHER_MOVE_DOWN) == 0,
	      "grid Down wraps from the last row to the first column");
	check(fplinux_launcher_move(10, 11, 3, FPLINUX_LAUNCHER_MOVE_DOWN) == 1,
	      "grid Down wraps to the same first-row column");
	check(fplinux_launcher_move(0, 10, 1, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "single-column grid Up wraps to the final app");
	check(fplinux_launcher_move(9, 10, 1, FPLINUX_LAUNCHER_MOVE_DOWN) == 0,
	      "single-column grid Down wraps to the first app");
	check(fplinux_launcher_move(8, 10, 3, FPLINUX_LAUNCHER_MOVE_DOWN) == 9,
	      "Down selects the final app in an incomplete row");
	check(fplinux_launcher_move(2, 10, 3, FPLINUX_LAUNCHER_MOVE_RIGHT) == 3,
	      "grid Right advances across a row boundary");
	for (unsigned int columns = 3; columns <= 4; ++columns) {
		check(fplinux_launcher_move(0, 10, columns,
					    FPLINUX_LAUNCHER_MOVE_LEFT) == 9,
		      "grid Left wraps to the final app");
		check(fplinux_launcher_move(9, 10, columns,
					    FPLINUX_LAUNCHER_MOVE_RIGHT) == 0,
		      "grid Right wraps to the first app");
	}
	check(fplinux_launcher_move(0, 10, 1, FPLINUX_LAUNCHER_MOVE_LEFT) == 9,
	      "single-column grid Left wraps to the final app");
	check(fplinux_launcher_move(9, 10, 1, FPLINUX_LAUNCHER_MOVE_RIGHT) == 0,
	      "single-column grid Right wraps to the first app");
	check(fplinux_launcher_move(2, 3, 3, FPLINUX_LAUNCHER_MOVE_UP) == 2,
	      "a single grid row keeps its column on Up");
	check(fplinux_launcher_move(2, 3, 3, FPLINUX_LAUNCHER_MOVE_DOWN) == 2,
	      "a single grid row keeps its column on Down");
	check(fplinux_launcher_digit_index(2, 3, 4, 10) == 9,
	      "phone digits count from the first visible row");
	check(fplinux_launcher_digit_index(2, 3, 5, 10) == 10,
	      "a digit beyond the installed catalogue selects nothing");
	check(fplinux_launcher_scroll_offset(&scroll, 5, 44, 0, 2) == 18,
	      "selection scrolls by only the missing pixels");
	check(fplinux_launcher_scroll_offset(&scroll, 5, 44, 18, 0) == 0,
	      "the first catalogue row returns to the top");
	check(fplinux_launcher_compare_names("Ель", "Ёж") < 0 &&
		      fplinux_launcher_compare_names("Ёж", "Жук") < 0,
	      "Russian names place Yo after Ye");
	check(fplinux_launcher_compare_names("Alpha", "beta") < 0,
	      "Latin sorting ignores case");
}

static bool inside(struct fplinux_launcher_rect rect, int width, int height)
{
	return rect.width > 0 && rect.height > 0 && rect.x >= 0 &&
	       rect.y >= 0 && rect.x + rect.width <= width &&
	       rect.y + rect.height <= height;
}

static int right(struct fplinux_launcher_rect rect)
{
	return rect.x + rect.width;
}

static int bottom(struct fplinux_launcher_rect rect)
{
	return rect.y + rect.height;
}

/* Equal space on both sides of rect in outer, horizontally or vertically. */
static bool centred_x(struct fplinux_launcher_rect rect,
		      struct fplinux_launcher_rect outer)
{
	return rect.x - outer.x == right(outer) - right(rect);
}

static bool centred_y(struct fplinux_launcher_rect rect,
		      struct fplinux_launcher_rect outer)
{
	return rect.y - outer.y == bottom(outer) - bottom(rect);
}

/*
 * rect is centred in outer's height to the pixel; an odd remainder is the
 * one row below it.
 */
static bool centred_y_odd_below(struct fplinux_launcher_rect rect,
				struct fplinux_launcher_rect outer)
{
	int above = rect.y - outer.y;
	int below = bottom(outer) - bottom(rect);

	return below - above == 0 || below - above == 1;
}

static bool same_rect(struct fplinux_launcher_rect a,
		      struct fplinux_launcher_rect b)
{
	return a.x == b.x && a.y == b.y && a.width == b.width &&
	       a.height == b.height;
}

/* The four 1 px edges cover the body's border except its corner pixels. */
static void check_outline(const struct fplinux_launcher_rect edges[4],
			  struct fplinux_launcher_rect body)
{
	struct fplinux_launcher_rect top = { body.x + 1, body.y, body.width - 2,
					     1 };
	struct fplinux_launcher_rect side = { body.x, body.y + 1, 1,
					      body.height - 2 };
	struct fplinux_launcher_rect lower = top;
	struct fplinux_launcher_rect opposite = side;

	lower.y = bottom(body) - 1;
	opposite.x = right(body) - 1;
	check(same_rect(edges[0], top) && same_rect(edges[1], lower) &&
		      same_rect(edges[2], side) &&
		      same_rect(edges[3], opposite),
	      "the outline leaves out exactly its corner pixels");
}

/*
 * The bolt has one row per pixel of its odd height, a full-width middle row,
 * 1 px strokes from its top right and to its bottom left that step one
 * column per row, and the same shape turned half a turn.
 */
static void check_bolt(const struct fplinux_launcher_battery *battery)
{
	struct fplinux_launcher_rect box = battery->bolt;
	const struct fplinux_launcher_rect *rows = battery->bolt_rows;
	int count = battery->bolt_row_count;
	int middle = count / 2;
	bool shaped = count == box.height && count % 2 &&
		      count <= FPLINUX_LAUNCHER_BOLT_ROWS_MAX &&
		      box.width == middle + 1;

	for (int row = 0; shaped && row < count; ++row) {
		const struct fplinux_launcher_rect *turned =
			&rows[count - 1 - row];

		shaped = rows[row].y == box.y + row && rows[row].height == 1 &&
			 rows[row].x >= box.x &&
			 right(rows[row]) <= right(box) &&
			 rows[row].width == (row == middle ? box.width : 1) &&
			 rows[row].x - box.x == right(box) - right(*turned);
		if (shaped && row && row != middle && row != middle + 1)
			shaped = rows[row].x == rows[row - 1].x - 1;
	}
	check(shaped && rows[0].x == right(box) - 1 &&
		      rows[count - 1].x == box.x,
	      "the bolt is a crisp zigzag symmetric about its centre");
}

/*
 * The drawn battery is pixel-exact: the bars are 1 px from the 1 px outline
 * on all four sides and 1 px apart, the cap and the "!" are centred without
 * a half-pixel remainder, and the bolt sits gap_small left of the body.
 * The body, the bolt and the lock glyph are centred in the status rows above
 * the bar's divider, so its row does not count as space below them.
 */
static void check_status_items(const struct fplinux_ui_profile_spec *spec,
			       int width, int status_height)
{
	struct fplinux_launcher_battery battery;
	struct fplinux_launcher_rect status = {
		.width = width,
		.height = status_height - spec->divider_width,
	};
	struct fplinux_launcher_rect glyph =
		fplinux_launcher_status_glyph(spec);
	struct fplinux_launcher_rect body;
	struct fplinux_launcher_rect *bars = battery.bars;
	struct fplinux_launcher_rect inner;
	int last = FPLINUX_LAUNCHER_BATTERY_STEP_COUNT - 1;
	bool even = true;

	fplinux_launcher_battery_layout(spec, width, &battery);
	body = battery.body;
	inner = (struct fplinux_launcher_rect){
		.x = body.x + 1,
		.y = body.y + 1,
		.width = body.width - 2,
		.height = body.height - 2,
	};
	check(inside(body, width, status.height) &&
		      inside(battery.cap, width, status.height) &&
		      inside(battery.bolt, width, status.height) &&
		      inside(glyph, width, status.height),
	      "status-bar items stay above the divider");
	check(right(battery.cap) == width - spec->margin &&
		      battery.cap.x == right(body),
	      "the cap ends the icon at the right margin");
	check(centred_y_odd_below(body, status),
	      "the body is centred in the status rows");
	check(centred_y_odd_below(glyph, status),
	      "the lock glyph is centred in the status rows");
	check(centred_y(battery.cap, body), "the cap is centred on the body");
	check(bars[0].x - inner.x == 1 && right(inner) - right(bars[last]) == 1,
	      "bars are 1 px from the outline at both ends");
	for (int i = 0; i <= last; ++i) {
		even = even && bars[i].y - inner.y == 1 &&
		       bottom(inner) - bottom(bars[i]) == 1 &&
		       bars[i].width == bars[0].width;
		if (i)
			even = even && bars[i].x - right(bars[i - 1]) == 1;
	}
	check(even, "bars are 1 px from the outline above and below and apart");
	check(centred_x(battery.low_stem, body) &&
		      battery.low_dot.x == battery.low_stem.x &&
		      battery.low_dot.width == battery.low_stem.width,
	      "the low mark is centred across the body");
	check(battery.low_stem.y == bars[0].y &&
		      battery.low_dot.y == bottom(battery.low_stem) + 1 &&
		      bottom(battery.low_dot) == bottom(bars[0]),
	      "the low mark fills the bars' height with a 1 px gap");
	check(right(battery.bolt) + spec->gap_small == body.x,
	      "the bolt is gap_small left of the body");
	check_outline(battery.edges, body);
	check_bolt(&battery);
	check(centred_y_odd_below(battery.bolt, status),
	      "the bolt is centred in the status rows");
}

/*
 * Home's analog stack, a dial over a date and three event lines, keeps
 * home_gap from both bars and between its parts; the dial is even so that
 * its centre falls between pixels, and no wider than the text.
 */
static void check_home_dial(const struct fplinux_ui_profile_spec *spec,
			    int width, int body_height)
{
	int line = 14;
	int date = line;
	int hint = 3 * line + 2 * spec->gap_small;
	int dial = fplinux_launcher_home_dial(spec, width, body_height, date,
					      hint);
	int stack = dial + spec->home_gap + date + spec->home_gap + hint;
	struct fplinux_launcher_home home;

	fplinux_launcher_home_layout(spec, body_height, dial, date, hint,
				     &home);
	check(dial > 0 && dial % 2 == 0 && dial <= width - 2 * spec->margin,
	      "the dial is an even square within the text width");
	check(home.clock_y >= spec->home_gap &&
		      home.hint_y + hint <= body_height - spec->home_gap,
	      "the analog stack keeps home_gap from both bars");
	check(body_height - 2 * spec->home_gap - stack <= 1 ||
		      dial == (width - 2 * spec->margin) / 2 * 2,
	      "the dial takes the rest of the body");
}

static void test_geometry(void)
{
	static const int screens[][2] = {
		{ 128, 128 }, { 128, 160 }, { 176, 220 },
		{ 240, 320 }, { 320, 240 },
	};

	for (size_t i = 0; i < sizeof(screens) / sizeof(*screens); ++i) {
		int width = screens[i][0];
		int height = screens[i][1];
		const struct fplinux_ui_profile_spec *spec =
			fplinux_ui_profile_spec(
				fplinux_ui_profile_for_screen(width, height));
		struct fplinux_launcher_frame frame;
		struct fplinux_launcher_launch launch;
		struct fplinux_launcher_scroll scroll;
		struct fplinux_launcher_cell cell;

		check(spec != NULL, "a supported screen has a layout profile");
		if (!spec)
			continue;
		fplinux_launcher_frame(spec, width, height, &frame);
		check(frame.width == width && frame.height == height,
		      "actual width and height remain independent");
		check(frame.body_y + frame.body_height + frame.softkey_height ==
			      height,
		      "body and bars fill the screen without overlap");
		check_status_items(spec, width, frame.status_height);
		check_home_dial(spec, width, frame.body_height);
		fplinux_launcher_scroll_layout(spec, width,
					       frame.viewport_height, &scroll);
		for (size_t column = 0; column < fplinux_launcher_columns(spec);
		     ++column) {
			fplinux_launcher_grid_cell(spec, width, &scroll, column,
						   0, &cell);
			check(inside(cell.tile.box, width,
				     frame.viewport_height),
			      "first-row tiles remain in the catalogue viewport");
		}
		fplinux_launcher_launch_layout(spec, &frame, &launch);
		check(inside(launch.icon, width, frame.body_height) &&
			      inside(launch.spinner, width,
				     frame.body_height) &&
			      inside(launch.text, width, frame.body_height),
		      "launch preview fits portrait and landscape bodies");
	}
	{
		const struct fplinux_ui_profile_spec *spec =
			fplinux_ui_profile_spec(
				fplinux_ui_profile_for_screen(320, 240));
		struct fplinux_launcher_scroll scroll;
		struct fplinux_launcher_cell cell;

		fplinux_launcher_scroll_layout(spec, 320, 170, &scroll);
		check(fplinux_launcher_content_height(&scroll, 2) == 170,
		      "two landscape rows fill the viewport without overflow");
		fplinux_launcher_grid_cell(spec, 320, &scroll, 0, 0, &cell);
		check(cell.tile.box.x == 16 && cell.tile.box.y == 17,
		      "landscape first tile has balanced horizontal and vertical margins");
		fplinux_launcher_grid_cell(spec, 320, &scroll, 3, 1, &cell);
		check(cell.tile.box.x == 244 && cell.tile.box.y == 93 &&
			      cell.tile.box.y + cell.tile.box.height == 153,
		      "landscape final tile leaves the same 17 pixel vertical margin");
		check(fplinux_launcher_content_height(&scroll, 3) > 170,
		      "larger catalogues still overflow and scroll");
	}
}

static void test_catalogue(const char *directory)
{
	struct fplinux_launcher_entry entry;

	check(fplinux_launcher_read_entry(directory, "org.test.notes.desktop",
					  "ru", getuid(), &entry),
	      "valid installed application is admitted");
	check(!strcmp(entry.id, "org.test.notes") &&
		      !strcmp(entry.name, "Заметки") && entry.icon_valid &&
		      !strcmp(entry.icon, "notes") &&
		      entry.group == FPLINUX_LAUNCHER_GROUP_ORGANISE,
	      "localized name, stable identity and declared group survive");
	check(fplinux_launcher_read_entry(directory,
					  "org.test.bad-icon.desktop", "ru",
					  getuid(), &entry) &&
		      !entry.icon_valid && !strcmp(entry.name, "Bad icon"),
	      "an invalid icon retains the application with a glyph fallback");
	check(!fplinux_launcher_read_entry(directory, "org.test.hidden.desktop",
					   "ru", getuid(), &entry),
	      "NoDisplay removes an entry from the catalogue");
	check(!fplinux_launcher_read_entry(directory, "org.test.unsafe.desktop",
					   "ru", getuid(), &entry),
	      "group-writable metadata cannot enter the catalogue");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "text"))
		test_text();
	else if (argc == 2 && !strcmp(argv[1], "navigation"))
		test_navigation();
	else if (argc == 2 && !strcmp(argv[1], "geometry"))
		test_geometry();
	else if (argc == 3 && !strcmp(argv[1], "catalogue"))
		test_catalogue(argv[2]);
	else
		return 2;
	return failures ? 1 : 0;
}
