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
		N_("2–9: Press repeatedly to choose a letter. It stays highlighted until a pause or another key sends it.\n"
		   "1: punctuation. 0: space.\n"
		   "*: choose a symbol. Hold * to change the input language.\n"
		   "#: change letter case. Hold # to switch between letters and 123.\n"
		   "Hold a digit to type the digit.\n"
		   "OK: Enter. Dial: Tab completion.\n"
		   "Clear: cancel a pending letter or the modifiers, otherwise erase. Hold to repeat.\n"
		   "The header shows the input mode on the right, such as abc or 123."),
	},
	{
		N_("Options"),
		N_("Modifiers: Mark Ctrl, Alt or Shift and choose Done. They apply to the next key or letter and stay visible in the header until then. When space is tight, C, A and S stand for Ctrl, Alt and Shift.\n"
		   "Symbols and Special keys: type a symbol, Esc, Home, End, Delete, PageUp, PageDown or F1–F12. Special keys opens with Esc selected and shows armed modifiers in its header.\n"
		   "Interrupt, End input and Search send exactly Ctrl+C, Ctrl+D and Ctrl+R, then clear unused modifiers. A pending letter is sent first with its modifiers.\n"
		   "Font size: choose Compact (6×12) or Larger (8×16). The choice is saved."),
	},
	{
		N_("Scrollback"),
		N_("Options > Scrollback shows earlier output. Up/Down: scroll one line. Left/Right: scroll one page. The header says Scrollback; its upward-arrow counter shows the distance from live output. At zero, the arrows still scroll.\n"
		   "Back returns to the latest output. While the shell is running, typing returns too.\n"
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
		   "The red key returns to Home and keeps the shell running. Reopen it from Tasks; Close app stops its programs.\n"
		   "A successful shell exit closes Terminal. After an unsuccessful exit, read the final output or Scrollback, then choose Close."),
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
