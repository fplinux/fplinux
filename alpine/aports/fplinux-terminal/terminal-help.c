/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-help.h"

#include <string.h>

static const struct {
	const char *title;
	const char *text;
} sections[] = {
	{
		"Typing",
		"Use Left/Right to switch sections and Up/Down to scroll the text.\n"
		"2-9: Tap repeatedly to choose a letter. Pause or press another digit to send it.\n"
		"1: Choose punctuation. 0: Insert a space.\n"
		"*: Switch letter case. Hold * to switch between numbers and letters.\n"
		"#: Cancel a pending letter, otherwise erase. Hold # to clear the line at the Bash prompt.\n"
		"Centre: Enter. Dial: Tab completion.\n"
		"Menu: Choose symbols, special keys or the input language.",
	},
	{
		"Modifiers",
		"Hold left soft or choose Menu > Modifiers to open the strip.\n"
		"Left/Right: Select Ctrl, Alt or Shift. Centre: Toggle it.\n"
		"1: Toggle Ctrl. 2: Toggle Alt. 3: Toggle Shift. You can combine them.\n"
		"Left soft (Done): Arm the combination for the next phone key or completed letter.\n"
		"Right soft (Cancel or Clear): Reset the combination outside menus.\n"
		"Back in a menu keeps modifiers armed. Pending letters wait while menus are open.",
	},
	{
		"History",
		"Right soft: Open scrollback when no modifiers are armed.\n"
		"Up/Down: Scroll one line. Left/Right: Scroll one page.\n"
		"Right soft (Back): Return to live output. Typing also resumes live input.\n"
		"External keyboard: Shift+PageUp and Shift+PageDown scroll output.\n"
		"At the Bash prompt, Up/Down choose previous or next commands.\n"
		"Menu > Search sends Ctrl+R to search command history.",
	},
};

/* Help is ASCII; newlines separate controls, not prewrapped display rows. */
static size_t next_line(const char **cursor, unsigned int columns)
{
	const char *text = *cursor;
	size_t size = 0;
	size_t last_space = 0;

	while (text[size] && text[size] != '\n' && size < columns) {
		if (text[size] == ' ')
			last_space = size;
		++size;
	}
	if (size == columns && text[size] && text[size] != '\n' &&
	    text[size] != ' ' && last_space)
		size = last_space;
	*cursor = text + size;
	while (**cursor == ' ')
		++*cursor;
	if (**cursor == '\n')
		++*cursor;
	return size;
}

void fplinux_terminal_help_open(const struct fplinux_terminal *terminal,
				struct fplinux_terminal_help *help)
{
	unsigned int rows = tsm_screen_get_height(terminal->screen);
	unsigned int line;
	const char *cursor;

	memset(help, 0, sizeof(*help));
	help->columns = tsm_screen_get_width(terminal->screen);
	/* The native grid already excludes the toolbar; the title uses a row. */
	if (terminal->modifier || terminal->modifier_panel)
		--rows;
	help->body_rows = rows > 1 ? rows - 1 : 1;
	help->section_count = sizeof(sections) / sizeof(sections[0]);
	help->section = terminal->menu_index % help->section_count;
	help->title = sections[help->section].title;
	help->cursor = sections[help->section].text;
	cursor = help->cursor;
	while (*cursor) {
		next_line(&cursor, help->columns);
		++help->line_count;
	}
	help->max_offset = help->line_count > help->body_rows ?
				   help->line_count - help->body_rows :
				   0;
	help->offset = terminal->help_scroll < help->max_offset ?
			       terminal->help_scroll :
			       help->max_offset;
	for (line = 0; line < help->offset; ++line)
		next_line(&help->cursor, help->columns);
	help->remaining_rows = help->body_rows;
}

size_t fplinux_terminal_help_next(struct fplinux_terminal_help *help,
				  const char **text)
{
	if (!help->remaining_rows || !*help->cursor)
		return 0;
	--help->remaining_rows;
	*text = help->cursor;
	return next_line(&help->cursor, help->columns);
}
