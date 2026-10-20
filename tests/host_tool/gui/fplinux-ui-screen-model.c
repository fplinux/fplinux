/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-ui-screen-model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void command_assignment_keeps_back_and_remaining_actions(void)
{
	const struct fplinux_ui_command_entry commands[] = {
		{ FPLINUX_UI_ROLE_OTHER, 3 }, { FPLINUX_UI_ROLE_OK, 0 },
		{ FPLINUX_UI_ROLE_BACK, 2 },  { FPLINUX_UI_ROLE_OTHER, 1 },
		{ FPLINUX_UI_ROLE_HELP, 4 },
	};
	struct fplinux_ui_command_keys keys;
	size_t menu[5];

	fplinux_ui_command_keys_assign(commands, 5, false, false, &keys, menu);
	assert(keys.right == 2 && keys.ok == 1 && keys.left == SIZE_MAX);
	assert(keys.options && keys.menu_count == 3);
	assert(menu[0] == 3 && menu[1] == 0 && menu[2] == 4);

	/* Editing uses OK and Clear; leaving remains available in Options. */
	fplinux_ui_command_keys_assign(commands, 5, true, true, &keys, menu);
	assert(keys.right == SIZE_MAX && keys.ok == SIZE_MAX);
	assert(keys.options && keys.menu_count == 5);
	assert(menu[0] == 1 && menu[1] == 3 && menu[2] == 2);
	assert(menu[3] == 0 && menu[4] == 4);

	/* Item commands precede screen commands when their priorities tie. */
	fplinux_ui_command_keys_assign(
		(const struct fplinux_ui_command_entry[]){
			{ FPLINUX_UI_ROLE_OTHER, 1 },
			{ FPLINUX_UI_ROLE_OTHER, 1 },
			{ FPLINUX_UI_ROLE_BACK, 0 },
		},
		3, false, false, &keys, menu);
	assert(keys.right == 2 && keys.menu_count == 2);
	assert(menu[0] == 0 && menu[1] == 1);

	/* A lone back action displaced by Clear still opens Options. */
	fplinux_ui_command_keys_assign(
		(const struct fplinux_ui_command_entry[]){
			{ FPLINUX_UI_ROLE_BACK, 0 },
		},
		1, false, true, &keys, menu);
	assert(keys.right == SIZE_MAX && keys.options);
	assert(keys.menu_count == 1 && menu[0] == 0);

	fplinux_ui_command_keys_assign(
		(const struct fplinux_ui_command_entry[]){
			{ FPLINUX_UI_ROLE_OTHER, 0 },
		},
		1, false, false, &keys, menu);
	assert(keys.left == 0 && !keys.options);
	fplinux_ui_command_keys_collapse(&keys, menu);
	assert(keys.left == SIZE_MAX && keys.options);
	assert(keys.menu_count == 1 && menu[0] == 0);
}

static bool selectable(size_t index, void *context)
{
	const bool *items = context;

	return items[index];
}

static void focus_follows_items_and_stops_at_list_edges(void)
{
	bool items[] = { false, true, false, true, false };

	assert(fplinux_ui_focus_next(2, 5, selectable, items) == 3);
	assert(fplinux_ui_focus_next(4, 5, selectable, items) == 3);
	items[1] = items[3] = false;
	assert(fplinux_ui_focus_next(2, 5, selectable, items) == SIZE_MAX);
	assert(fplinux_ui_index_inserted(2, 1, 3) == 5);
	assert(fplinux_ui_index_inserted(SIZE_MAX, 1, 3) == SIZE_MAX);
	assert(fplinux_ui_focus_deleted(2, 2, 1, 5) == 2);
	assert(fplinux_ui_focus_deleted(4, 3, 2, 5) == 2);
	assert(fplinux_ui_focus_deleted(1, 0, 5, 5) == SIZE_MAX);
	assert(fplinux_ui_index_deleted(2, 2, 1) == SIZE_MAX);
	assert(fplinux_ui_focus_step(2, 10, 4) == 6);
	assert(fplinux_ui_focus_step(2, 10, -4) == 0);
	assert(fplinux_ui_focus_step(8, 10, 4) == 9);
	assert(fplinux_ui_focus_step(SIZE_MAX, 10, 1) == 0);
	assert(fplinux_ui_focus_step(SIZE_MAX, 0, 1) == SIZE_MAX);
	assert(fplinux_ui_reveal_scroll(10, 109, 5, 20) == 5);
	assert(fplinux_ui_reveal_scroll(10, 109, 95, 120) == -11);
	assert(fplinux_ui_reveal_scroll(10, 109, 25, 40) == 0);
}

static void virtual_focus_moves_scroll_and_survives_row_changes(void)
{
	struct fplinux_ui_virtual_window window;
	size_t changed[FPLINUX_UI_VIRTUAL_SLOTS_MAX];

	fplinux_ui_virtual_init(&window, 100, 20, 60, changed);
	assert(window.focus == 0 && window.top == 0);
	fplinux_ui_virtual_set_focus(
		&window, fplinux_ui_focus_step(window.focus, window.count, 3),
		changed);
	assert(window.focus == 3 && window.top == 20);
	fplinux_ui_virtual_set_focus(&window, 99, changed);
	assert(window.focus == 99 && window.top == 1940);
	fplinux_ui_virtual_set_focus(
		&window, fplinux_ui_focus_step(window.focus, window.count, 1),
		changed);
	assert(window.focus == 99 && window.top == 1940);
	fplinux_ui_virtual_set_focus(
		&window, fplinux_ui_focus_step(window.focus, window.count, -3),
		changed);
	assert(window.focus == 96 && window.top == 1920);

	/* The same row keeps its place when rows are inserted above it. */
	fplinux_ui_virtual_inserted(&window, 0, 2, changed);
	assert(window.count == 102 && window.focus == 98 && window.top == 1960);
	fplinux_ui_virtual_deleted(&window, 0, 2, changed);
	assert(window.count == 100 && window.focus == 96 && window.top == 1920);
	fplinux_ui_virtual_deleted(&window, 95, 5, changed);
	assert(window.count == 95 && window.focus == 94 && window.top == 1840);
	fplinux_ui_virtual_resize(&window, 20, 100, changed);
	assert(window.focus == 94 && window.top == 1800);
	fplinux_ui_virtual_deleted(&window, 0, 95, changed);
	assert(window.count == 0 && window.focus == SIZE_MAX &&
	       window.top == 0);
	fplinux_ui_virtual_inserted(&window, 0, 1, changed);
	assert(window.count == 1 && window.focus == 0 && window.top == 0);
}

static void shows_date(const struct fplinux_ui_date_edit *edit,
		       const char *expected, size_t start, size_t end)
{
	char text[64];
	size_t actual_start, actual_end;

	fplinux_ui_date_edit_format(edit, text, sizeof(text), &actual_start,
				    &actual_end);
	assert(!strcmp(text, expected));
	assert(actual_start == start && actual_end == end);
}

static void date_editing_keeps_the_day_and_formats_typed_time(void)
{
	struct tm january = { .tm_year = 2024 - 1900,
			      .tm_mon = 0,
			      .tm_mday = 31,
			      .tm_hour = 14,
			      .tm_min = 5,
			      .tm_sec = 17 };
	time_t value;
	struct fplinux_ui_date_edit edit;
	struct tm *result;

	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	value = mktime(&january);
	fplinux_ui_date_edit_begin(&edit, value, true, true,
				   "{dd}.{mm}.{yyyy}");
	shows_date(&edit, "31.01.2024 14:05", 0, 2);
	fplinux_ui_date_edit_move(&edit, 1);
	fplinux_ui_date_edit_step(&edit, 1);
	shows_date(&edit, "29.02.2024 14:05", 3, 5);
	fplinux_ui_date_edit_step(&edit, 1);
	shows_date(&edit, "31.03.2024 14:05", 3, 5);
	fplinux_ui_date_edit_move(&edit, 1);
	fplinux_ui_date_edit_digit(&edit, 2);
	fplinux_ui_date_edit_digit(&edit, 0);
	fplinux_ui_date_edit_digit(&edit, 2);
	fplinux_ui_date_edit_digit(&edit, 5);
	shows_date(&edit, "31.03.2025 14:05", 11, 13);
	fplinux_ui_date_edit_digit(&edit, 0);
	fplinux_ui_date_edit_digit(&edit, 9);
	fplinux_ui_date_edit_digit(&edit, 3);
	fplinux_ui_date_edit_digit(&edit, 0);
	shows_date(&edit, "31.03.2025 09:30", 14, 16);
	value = fplinux_ui_date_edit_value(&edit);
	result = localtime(&value);
	assert(result && result->tm_year == 125 && result->tm_mon == 2);
	assert(result->tm_mday == 31 && result->tm_hour == 9);
	assert(result->tm_min == 30 && result->tm_sec == 17);

	fplinux_ui_date_edit_begin(&edit, value, true, false,
				   "{mm}/{dd}/{yyyy}");
	shows_date(&edit, "03/31/2025", 0, 2);
	fplinux_ui_date_edit_begin(&edit, value, true, false, "{dd}/{dd}");
	shows_date(&edit, "31.03.2025", 0, 2);
	fplinux_ui_date_edit_begin(&edit, value, false, true, NULL);
	shows_date(&edit, "09:30", 0, 2);
}

int main(void)
{
	command_assignment_keeps_back_and_remaining_actions();
	focus_follows_items_and_stops_at_list_edges();
	virtual_focus_moves_scroll_and_survives_row_changes();
	date_editing_keeps_the_day_and_formats_typed_time();
	puts("screen model: OK");
	return 0;
}
