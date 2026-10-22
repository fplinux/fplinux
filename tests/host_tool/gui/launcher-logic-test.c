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

	check(fplinux_launcher_move(0, 10, 3, 2, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "grid Up wraps from the first row into the last row");
	check(fplinux_launcher_move(2, 10, 3, 2, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "grid Up clamps a missing last-row column to the final app");
	check(fplinux_launcher_move(1, 11, 3, 2, FPLINUX_LAUNCHER_MOVE_UP) ==
		      10,
	      "grid Up preserves the column when it exists in the last row");
	check(fplinux_launcher_move(9, 11, 3, 2, FPLINUX_LAUNCHER_MOVE_DOWN) ==
		      0,
	      "grid Down wraps from the last row to the first column");
	check(fplinux_launcher_move(10, 11, 3, 2, FPLINUX_LAUNCHER_MOVE_DOWN) ==
		      1,
	      "grid Down wraps to the same first-row column");
	check(fplinux_launcher_move(0, 10, 1, 4, FPLINUX_LAUNCHER_MOVE_UP) == 9,
	      "list Up wraps to the final app");
	check(fplinux_launcher_move(9, 10, 1, 4, FPLINUX_LAUNCHER_MOVE_DOWN) ==
		      0,
	      "list Down wraps to the first app");
	check(fplinux_launcher_move(8, 10, 3, 2, FPLINUX_LAUNCHER_MOVE_DOWN) ==
		      9,
	      "Down selects the final app in an incomplete row");
	check(fplinux_launcher_move(2, 10, 3, 2, FPLINUX_LAUNCHER_MOVE_RIGHT) ==
		      3,
	      "grid Right advances across a row boundary");
	check(fplinux_launcher_move(3, 10, 1, 4, FPLINUX_LAUNCHER_MOVE_RIGHT) ==
		      7,
	      "list Right advances one page");
	check(fplinux_launcher_move(7, 10, 1, 4, FPLINUX_LAUNCHER_MOVE_LEFT) ==
		      3,
	      "list Left returns one page");
	check(fplinux_launcher_move(0, 10, 1, 1, FPLINUX_LAUNCHER_MOVE_LEFT) ==
		      0,
	      "one-row list Left page stops at the first app");
	check(fplinux_launcher_move(9, 10, 1, 1, FPLINUX_LAUNCHER_MOVE_RIGHT) ==
		      9,
	      "one-row list Right page stops at the final app");
	check(fplinux_launcher_move(2, 3, 3, 2, FPLINUX_LAUNCHER_MOVE_UP) == 2,
	      "a single grid row keeps its column on Up");
	check(fplinux_launcher_move(2, 3, 3, 2, FPLINUX_LAUNCHER_MOVE_DOWN) ==
		      2,
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

static void test_geometry(void)
{
	static const int screens[][4] = {
		{ 128, 128, 4, 120 }, { 128, 160, 4, 120 },
		{ 176, 220, 6, 164 }, { 240, 320, 8, 224 },
		{ 320, 240, 6, 308 },
	};

	for (size_t i = 0; i < sizeof(screens) / sizeof(*screens); ++i) {
		int width = screens[i][0];
		int height = screens[i][1];
		const struct fplinux_ui_profile_spec *spec =
			fplinux_ui_profile_spec(
				fplinux_ui_profile_for_screen(width, height));
		struct fplinux_launcher_frame frame;
		struct fplinux_launcher_battery battery;
		struct fplinux_launcher_launch launch;
		struct fplinux_launcher_scroll scroll;
		struct fplinux_launcher_cell cell;
		struct fplinux_launcher_row row;
		struct fplinux_launcher_notice_box notice;

		check(spec != NULL, "a supported screen has a layout profile");
		if (!spec)
			continue;
		fplinux_launcher_frame(spec, width, height, &frame);
		check(frame.width == width && frame.height == height,
		      "actual width and height remain independent");
		check(frame.body_y + frame.body_height + frame.softkey_height ==
			      height,
		      "body and bars fill the screen without overlap");
		fplinux_launcher_battery_layout(spec, width, &battery);
		check(inside(battery.body, width, frame.status_height) &&
			      inside(battery.cap, width, frame.status_height) &&
			      inside(battery.bolt, width, frame.status_height),
		      "battery and charging shapes stay in the status bar");
		fplinux_launcher_scroll_layout(
			spec, width, frame.viewport_height, true, &scroll);
		for (size_t column = 0;
		     column < fplinux_launcher_columns(spec, true); ++column) {
			fplinux_launcher_grid_cell(spec, width, &scroll, column,
						   0, &cell);
			check(inside(cell.tile.box, width,
				     frame.viewport_height),
			      "first-row tiles remain in the catalogue viewport");
		}
		fplinux_launcher_list_row(spec, width, 12, 0, &row);
		check(row.row.x == screens[i][2] &&
			      row.row.width == screens[i][3] &&
			      width - row.row.x - row.row.width ==
				      screens[i][2],
		      "catalogue rows have equal left and right outer margins");
		check(inside(row.row, width, frame.viewport_height) &&
			      inside(row.icon.box, row.row.width,
				     row.row.height) &&
			      inside(row.text, row.row.width, row.row.height),
		      "catalogue list contents remain within the row");
		fplinux_launcher_launch_layout(spec, &frame, &launch);
		check(inside(launch.icon, width, frame.body_height) &&
			      inside(launch.spinner, width,
				     frame.body_height) &&
			      inside(launch.text, width, frame.body_height),
		      "launch preview fits portrait and landscape bodies");
		fplinux_launcher_notice_layout(spec, width, frame.body_height,
					       &notice);
		check(notice.width > 0 && notice.width <= width &&
			      notice.max_height > 0 &&
			      notice.max_height <= frame.body_height,
		      "notice bounds fit the body");
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
