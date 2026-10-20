/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-help.h"
#include "fplinux-terminal.h"

struct help_section {
	const char *title;
	const char *text;
};

/* Translated when shown; the help screen wraps the lines. */
static const struct help_section sections[] = {
	{
		N_("Typing"),
		N_("2–9: Press repeatedly to choose a letter. Pause or press another key to send it.\n"
		   "1: punctuation. 0: space.\n"
		   "*: switch letter case. Hold *: switch between digits and letters.\n"
		   "#: cancel a pending letter, otherwise erase. Hold #: clear the line at the Bash prompt.\n"
		   "OK: Enter. Dial: Tab completion.\n"
		   "Clear: cancel a pending letter or the modifiers, otherwise erase.\n"
		   "The title shows the input mode, such as abc or 123."),
	},
	{
		N_("Options"),
		N_("Modifiers: Mark Ctrl, Alt or Shift and choose Done. They apply to the next key or letter, and the title shows them until then.\n"
		   "Symbols and Special keys: type a symbol, Esc or F1–F12.\n"
		   "Language: choose the letters of multi-tap.\n"
		   "Interrupt, End input and Search send Ctrl+C, Ctrl+D and Ctrl+R."),
	},
	{
		N_("Scrollback"),
		N_("Options > Scrollback shows earlier output. Up/Down: scroll one line. Left/Right: scroll one page. The title shows how many lines are above.\n"
		   "Back or typing returns to live output.\n"
		   "At the Bash prompt, Up/Down choose earlier commands, and Search finds one."),
	},
	{
		N_("Keyboard"),
		N_("An external keyboard types directly, with its own Ctrl, Alt and Shift, Esc and Enter.\n"
		   "Shift+PageUp and Shift+PageDown scroll the output.\n"
		   "Ctrl+Esc returns to Home and keeps the shell running."),
	},
	{
		N_("Exit"),
		N_("Options > Exit closes the terminal and its shell and returns to the app list. Save files first.\n"
		   "The red key offers Minimize and Close. Minimize keeps the shell running; Close stops its programs. Tasks reopens a minimized terminal."),
	},
};

size_t fplinux_terminal_help_count(void)
{
	return sizeof(sections) / sizeof(sections[0]);
}

void fplinux_terminal_help_section(size_t index, const char **title,
				   const char **text)
{
	if (index >= fplinux_terminal_help_count()) {
		*title = "";
		*text = "";
		return;
	}
	*title = _(sections[index].title);
	*text = _(sections[index].text);
}
