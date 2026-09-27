/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"
#include "terminal-help.h"

#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define FPLINUX_TERMINAL_POUND_HOLD_MS 650U
#define FPLINUX_TERMINAL_STAR_HOLD_MS 650U
#define FPLINUX_TERMINAL_SOFTLEFT_HOLD_MS 500U
#define FPLINUX_TERMINAL_CURSOR_MS 600U

enum main_menu_item {
	MENU_MODIFIERS,
	MENU_SYMBOLS,
	MENU_SPECIAL,
	MENU_LANGUAGE,
	MENU_INTERRUPT,
	MENU_END_INPUT,
	MENU_SEARCH,
	MENU_HELP,
	MENU_DIAGNOSTIC,
};

static const char *const main_menu[] = {
	[MENU_MODIFIERS] = "Modifiers",
	[MENU_SYMBOLS] = "Symbols...",
	[MENU_SPECIAL] = "Special keys...",
	[MENU_LANGUAGE] = "Language: EN...",
	[MENU_INTERRUPT] = "Interrupt Ctrl+C",
	[MENU_END_INPUT] = "End input Ctrl+D",
	[MENU_SEARCH] = "Search Ctrl+R",
	[MENU_HELP] = "Help",
	[MENU_DIAGNOSTIC] = "Diagnostic console",
};
static const char *const symbols[] = {
	"#", "*", "|", "&", "~",  "`", "\\", "<", ">", "[", "]",
	"{", "}", "(", ")", "\"", "'", ":",  ";", "!", "?", "@",
	"$", "%", "^", "_", "=",  "+", "-",  "/", ".", ",",
};
static const char *const special_keys[] = {
	"Esc", "F1", "F2", "F3",  "F4",	 "F5",	"F6",
	"F7",  "F8", "F9", "F10", "F11", "F12",
};
static const char *const languages[] = {
	"English (EN)",
	"Russian (RU)",
};

static uint32_t uppercase(uint32_t character)
{
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 'A';
	if (character >= 0x430 && character <= 0x44f)
		return character - 0x20;
	if (character == 0x451)
		return 0x401;
	return character;
}

static void send_character(struct fplinux_terminal *terminal,
			   uint32_t character)
{
	uint32_t ascii = character < 128 ? character : TSM_VTE_INVALID;
	uint32_t keysym;

	if (terminal->alphabetic_mode == FPLINUX_TERMINAL_UPPER ||
	    (terminal->modifier & TSM_SHIFT_MASK))
		character = uppercase(character);
	keysym = character <= 0xff ? character : 0x01000000U | character;
	tsm_vte_handle_keyboard(terminal->vte, keysym, ascii,
				terminal->modifier, character);
	terminal->modifier = 0;
	terminal->dirty = true;
}

static enum fplinux_multitap_emit_result emit_composition(void *data,
							  uint32_t character)
{
	struct fplinux_terminal *terminal = data;

	/* The event loop drains PTY input before accepting another key. */
	if (terminal->output_size > sizeof(terminal->output) - 32)
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	send_character(terminal, character);
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static bool commit_composition(struct fplinux_terminal *terminal)
{
	return fplinux_multitap_commit(&terminal->compose, emit_composition,
				       terminal) != FPLINUX_MULTITAP_BLOCKED;
}

static void resume_composition(struct fplinux_terminal *terminal,
			       uint64_t now_ms)
{
	if (!terminal->compose_resume_pending)
		return;
	terminal->compose_ms = now_ms;
	terminal->compose_resume_pending = false;
}

static void return_live(struct fplinux_terminal *terminal)
{
	tsm_screen_sb_reset(terminal->screen);
	terminal->history = false;
	terminal->dirty = true;
}

static void scroll(struct fplinux_terminal *terminal, uint32_t keysym)
{
	terminal->history = true;
	switch (keysym) {
	case XKB_KEY_Up:
		tsm_screen_sb_up(terminal->screen, 1);
		break;
	case XKB_KEY_Down:
		tsm_screen_sb_down(terminal->screen, 1);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_Page_Up:
		tsm_screen_sb_page_up(terminal->screen, 1);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_Page_Down:
		tsm_screen_sb_page_down(terminal->screen, 1);
		break;
	}
	terminal->dirty = true;
}

static void send_phone_key(struct fplinux_terminal *terminal, uint32_t keysym,
			   uint32_t character, unsigned int modifiers)
{
	if (!commit_composition(terminal)) {
		terminal->failed = true;
		return;
	}
	return_live(terminal);
	modifiers |= terminal->modifier;
	if (keysym == XKB_KEY_Tab && (modifiers & TSM_SHIFT_MASK))
		keysym = XKB_KEY_ISO_Left_Tab;
	tsm_vte_handle_keyboard(terminal->vte, keysym, character, modifiers,
				character);
	terminal->modifier = 0;
	terminal->cursor_visible = true;
}

static void open_modifiers(struct fplinux_terminal *terminal)
{
	return_live(terminal);
	terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
	terminal->modifier_panel = true;
	terminal->modifier_selection = terminal->modifier;
	terminal->modifier_index = 0;
	terminal->cursor_visible = true;
}

static void close_modifiers(struct fplinux_terminal *terminal, bool apply,
			    uint64_t now_ms)
{
	terminal->modifier = apply ? terminal->modifier_selection : 0;
	terminal->modifier_selection = 0;
	terminal->modifier_panel = false;
	terminal->compose_ms = now_ms;
	terminal->dirty = true;
}

static void modifier_key(struct fplinux_terminal *terminal, unsigned int code)
{
	static const unsigned int masks[] = {
		TSM_CONTROL_MASK,
		TSM_ALT_MASK,
		TSM_SHIFT_MASK,
	};

	if (code == KEY_LEFT)
		terminal->modifier_index = (terminal->modifier_index + 2) % 3;
	else if (code == KEY_RIGHT)
		terminal->modifier_index = (terminal->modifier_index + 1) % 3;
	else if (code == KEY_OK)
		terminal->modifier_selection ^= masks[terminal->modifier_index];
	else if (code >= KEY_NUMERIC_1 && code <= KEY_NUMERIC_3) {
		terminal->modifier_index = code - KEY_NUMERIC_1;
		terminal->modifier_selection ^= masks[terminal->modifier_index];
	}
	terminal->dirty = true;
}

static void set_mode(struct fplinux_terminal *terminal,
		     enum fplinux_terminal_mode mode)
{
	terminal->mode = mode;
	if (mode != FPLINUX_TERMINAL_NUMERIC)
		terminal->alphabetic_mode = mode;
	terminal->dirty = true;
}

static void hold_star(struct fplinux_terminal *terminal, uint64_t now_ms)
{
	set_mode(terminal, terminal->mode == FPLINUX_TERMINAL_NUMERIC ?
				   terminal->alphabetic_mode :
				   FPLINUX_TERMINAL_NUMERIC);
	terminal->star_held = true;
	terminal->compose_ms = now_ms;
}

static void select_menu(struct fplinux_terminal *terminal)
{
	unsigned int index = terminal->menu_index;
	enum fplinux_terminal_menu menu = terminal->menu;

	if (menu == FPLINUX_TERMINAL_MENU_HELP)
		return;
	terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
	terminal->menu_index = 0;
	if (menu == FPLINUX_TERMINAL_MENU_SYMBOLS) {
		if (commit_composition(terminal))
			send_character(terminal,
				       (unsigned char)symbols[index][0]);
	} else if (menu == FPLINUX_TERMINAL_MENU_SPECIAL) {
		send_phone_key(terminal,
			       index ? XKB_KEY_F1 + index - 1 : XKB_KEY_Escape,
			       TSM_VTE_INVALID, 0);
	} else if (menu == FPLINUX_TERMINAL_MENU_LANGUAGE) {
		if (!commit_composition(terminal))
			return;
		fplinux_multitap_set_language(&terminal->compose,
					      index == 0 ?
						      FPLINUX_MULTITAP_ENGLISH :
						      FPLINUX_MULTITAP_RUSSIAN);
	} else {
		switch (index) {
		case MENU_MODIFIERS:
			open_modifiers(terminal);
			break;
		case MENU_SYMBOLS:
			terminal->menu = FPLINUX_TERMINAL_MENU_SYMBOLS;
			break;
		case MENU_SPECIAL:
			terminal->menu = FPLINUX_TERMINAL_MENU_SPECIAL;
			break;
		case MENU_LANGUAGE:
			terminal->menu = FPLINUX_TERMINAL_MENU_LANGUAGE;
			terminal->menu_index = terminal->compose.language ==
					       FPLINUX_MULTITAP_RUSSIAN;
			break;
		case MENU_INTERRUPT:
			send_phone_key(terminal, XKB_KEY_c, 'c',
				       TSM_CONTROL_MASK);
			break;
		case MENU_END_INPUT:
			send_phone_key(terminal, XKB_KEY_d, 'd',
				       TSM_CONTROL_MASK);
			break;
		case MENU_SEARCH:
			send_phone_key(terminal, XKB_KEY_r, 'r',
				       TSM_CONTROL_MASK);
			break;
		case MENU_HELP:
			terminal->help_scroll = 0;
			terminal->menu = FPLINUX_TERMINAL_MENU_HELP;
			break;
		case MENU_DIAGNOSTIC:
			terminal->diagnostic_requested = true;
			break;
		}
	}
	terminal->dirty = true;
}

static void menu_back(struct fplinux_terminal *terminal)
{
	switch (terminal->menu) {
	case FPLINUX_TERMINAL_MENU_MAIN:
		terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
		return;
	case FPLINUX_TERMINAL_MENU_SYMBOLS:
		terminal->menu_index = MENU_SYMBOLS;
		break;
	case FPLINUX_TERMINAL_MENU_SPECIAL:
		terminal->menu_index = MENU_SPECIAL;
		break;
	case FPLINUX_TERMINAL_MENU_LANGUAGE:
		terminal->menu_index = MENU_LANGUAGE;
		break;
	case FPLINUX_TERMINAL_MENU_HELP:
		terminal->menu_index = MENU_HELP;
		break;
	default:
		terminal->menu_index = MENU_MODIFIERS;
		break;
	}
	terminal->menu = FPLINUX_TERMINAL_MENU_MAIN;
}

static bool menu_key(struct fplinux_terminal *terminal, uint32_t keysym)
{
	unsigned int count = fplinux_terminal_menu_count(terminal);

	if (terminal->menu == FPLINUX_TERMINAL_MENU_HELP) {
		struct fplinux_terminal_help help;

		fplinux_terminal_help_open(terminal, &help);
		if (keysym == XKB_KEY_Left || keysym == XKB_KEY_Right) {
			terminal->menu_index =
				(help.section +
				 (keysym == XKB_KEY_Left ?
					  help.section_count - 1 :
					  1)) %
				help.section_count;
			terminal->help_scroll = 0;
		} else if (keysym == XKB_KEY_Up) {
			terminal->help_scroll = help.offset ? help.offset - 1 :
							      0;
		} else if (keysym == XKB_KEY_Down) {
			terminal->help_scroll = help.offset < help.max_offset ?
							help.offset + 1 :
							help.max_offset;
		} else if (keysym == XKB_KEY_Escape)
			menu_back(terminal);
		terminal->dirty = true;
		return true;
	}
	if (!count)
		return false;
	terminal->dirty = true;
	if (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down) {
		terminal->menu_index =
			(terminal->menu_index +
			 (keysym == XKB_KEY_Up ? count - 1 : 1)) %
			count;
	} else if (keysym == XKB_KEY_Return)
		select_menu(terminal);
	else if (keysym == XKB_KEY_Escape || keysym == XKB_KEY_Left) {
		menu_back(terminal);
	} else {
		terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
		return false;
	}
	return true;
}

void fplinux_terminal_key(struct fplinux_terminal *terminal, uint32_t keysym,
			  uint32_t ascii, unsigned int modifiers,
			  uint32_t unicode)
{
	if (!terminal->active)
		return;
	if (menu_key(terminal, keysym)) {
		if (terminal->menu == FPLINUX_TERMINAL_MENU_CLOSED &&
		    fplinux_multitap_pending(&terminal->compose))
			terminal->compose_resume_pending = true;
		return;
	}
	if ((keysym == XKB_KEY_F1 || keysym == XKB_KEY_XF86Switch_VT_1) &&
	    (modifiers & (TSM_CONTROL_MASK | TSM_ALT_MASK)) ==
		    (TSM_CONTROL_MASK | TSM_ALT_MASK)) {
		terminal->diagnostic_requested = true;
		return;
	}
	if ((modifiers & ~TSM_LOCK_MASK) == TSM_SHIFT_MASK &&
	    (keysym == XKB_KEY_Page_Up || keysym == XKB_KEY_Page_Down)) {
		if (commit_composition(terminal))
			scroll(terminal, keysym);
		return;
	}
	return_live(terminal);
	if (!commit_composition(terminal)) {
		terminal->failed = true;
		return;
	}
	/* Phone one-shot modifiers never alter the external keyboard source. */
	tsm_vte_handle_keyboard(terminal->vte, keysym, ascii, modifiers,
				unicode);
	terminal->cursor_visible = true;
}

static uint32_t phone_keysym(unsigned int code)
{
	switch (code) {
	case KEY_UP:
		return XKB_KEY_Up;
	case KEY_DOWN:
		return XKB_KEY_Down;
	case KEY_LEFT:
		return XKB_KEY_Left;
	case KEY_RIGHT:
		return XKB_KEY_Right;
	case KEY_OK:
		return XKB_KEY_Return;
	case KEY_PICKUP_PHONE:
		return XKB_KEY_Tab;
	default:
		return XKB_KEY_NoSymbol;
	}
}

static void clear_line(struct fplinux_terminal *terminal, bool shell_foreground)
{
	fplinux_multitap_cancel(&terminal->compose);
	terminal->modifier = 0;
	terminal->pound_held = true;
	terminal->dirty = true;
	if (!shell_foreground || !terminal->editing)
		return;
	if (sizeof(FPLINUX_TERMINAL_CLEAR_LINE) - 1 >
	    sizeof(terminal->output) - terminal->output_size) {
		terminal->failed = true;
		return;
	}
	memcpy(terminal->output + terminal->output_size,
	       FPLINUX_TERMINAL_CLEAR_LINE,
	       sizeof(FPLINUX_TERMINAL_CLEAR_LINE) - 1);
	terminal->output_size += sizeof(FPLINUX_TERMINAL_CLEAR_LINE) - 1;
}

void fplinux_terminal_phone(struct fplinux_terminal *terminal,
			    unsigned int code, bool pressed, bool repeat,
			    uint64_t now_ms, bool shell_foreground)
{
	uint32_t keysym = phone_keysym(code);

	if (!terminal->active)
		return;
	resume_composition(terminal, now_ms);
	if (terminal->menu == FPLINUX_TERMINAL_MENU_HELP) {
		if (pressed) {
			menu_key(terminal,
				 code == KEY_F14 ? XKB_KEY_Escape : keysym);
			terminal->compose_ms = now_ms;
		}
		return;
	}
	if (code == KEY_F13) {
		if (repeat)
			return;
		if (pressed) {
			terminal->softleft_down = true;
			terminal->softleft_held = false;
			terminal->softleft_ms = now_ms;
		} else if (terminal->softleft_down) {
			if (!terminal->softleft_held &&
			    !terminal->modifier_panel &&
			    now_ms - terminal->softleft_ms >=
				    FPLINUX_TERMINAL_SOFTLEFT_HOLD_MS) {
				open_modifiers(terminal);
				terminal->softleft_held = true;
			}
			if (!terminal->softleft_held) {
				if (terminal->modifier_panel)
					close_modifiers(terminal, true, now_ms);
				else if (terminal->menu !=
					 FPLINUX_TERMINAL_MENU_CLOSED)
					select_menu(terminal);
				else {
					return_live(terminal);
					terminal->menu =
						FPLINUX_TERMINAL_MENU_MAIN;
					terminal->menu_index = MENU_MODIFIERS;
				}
			}
			terminal->softleft_down = false;
			terminal->compose_ms = now_ms;
			terminal->dirty = true;
		}
		return;
	}
	if (terminal->modifier_panel) {
		if (pressed && !repeat) {
			if (code == KEY_F14)
				close_modifiers(terminal, false, now_ms);
			else
				modifier_key(terminal, code);
		}
		return;
	}
	if (code == KEY_NUMERIC_STAR) {
		if (repeat)
			return;
		if (pressed) {
			return_live(terminal);
			terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
			terminal->star_down = true;
			terminal->star_held = false;
			terminal->star_ms = now_ms;
		} else if (terminal->star_down) {
			if (!terminal->star_held &&
			    now_ms - terminal->star_ms >=
				    FPLINUX_TERMINAL_STAR_HOLD_MS)
				hold_star(terminal, now_ms);
			if (!terminal->star_held)
				set_mode(
					terminal,
					terminal->alphabetic_mode ==
							FPLINUX_TERMINAL_LOWER ?
						FPLINUX_TERMINAL_UPPER :
						FPLINUX_TERMINAL_LOWER);
			terminal->star_down = false;
			terminal->compose_ms = now_ms;
		}
		return;
	}
	if (code == KEY_NUMERIC_POUND) {
		if (repeat)
			return;
		if (pressed) {
			return_live(terminal);
			terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
			terminal->pound_down = true;
			terminal->pound_held = false;
			terminal->pound_cancelled =
				fplinux_multitap_pending(&terminal->compose);
			fplinux_multitap_cancel(&terminal->compose);
			terminal->pound_ms = now_ms;
		} else if (terminal->pound_down) {
			if (!terminal->pound_held &&
			    now_ms - terminal->pound_ms >=
				    FPLINUX_TERMINAL_POUND_HOLD_MS)
				clear_line(terminal, shell_foreground);
			if (!terminal->pound_held) {
				if (!terminal->pound_cancelled)
					send_phone_key(terminal,
						       XKB_KEY_BackSpace,
						       TSM_VTE_INVALID, 0);
				terminal->modifier = 0;
			}
			terminal->pound_down = false;
			terminal->dirty = true;
		}
		terminal->dirty = true;
		return;
	}
	if (!pressed)
		return;
	if (code == KEY_F14) {
		if (repeat)
			return;
		if (terminal->menu != FPLINUX_TERMINAL_MENU_CLOSED) {
			menu_key(terminal, XKB_KEY_Escape);
			terminal->compose_ms = now_ms;
		} else if (terminal->modifier) {
			terminal->modifier = 0;
			terminal->dirty = true;
		} else if (terminal->history) {
			return_live(terminal);
		} else if (commit_composition(terminal)) {
			terminal->history = true;
			terminal->dirty = true;
		}
		return;
	}
	if (terminal->history &&
	    (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down ||
	     keysym == XKB_KEY_Left || keysym == XKB_KEY_Right)) {
		scroll(terminal, keysym);
		return;
	}
	if (terminal->menu != FPLINUX_TERMINAL_MENU_CLOSED &&
	    menu_key(terminal, keysym)) {
		terminal->compose_ms = now_ms;
		return;
	}
	return_live(terminal);
	if (code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9) {
		unsigned char character =
			(unsigned char)('0' + code - KEY_NUMERIC_0);
		/* Resuming from a menu may precede delivery of an older event. */
		uint64_t elapsed = now_ms >= terminal->compose_ms ?
					   now_ms - terminal->compose_ms :
					   0;

		if (repeat)
			return;
		if (terminal->mode == FPLINUX_TERMINAL_NUMERIC) {
			if (commit_composition(terminal))
				send_character(terminal, character);
		} else {
			fplinux_multitap_press(&terminal->compose, character,
					       elapsed > UINT32_MAX ?
						       UINT32_MAX :
						       (uint32_t)elapsed,
					       emit_composition, terminal);
			terminal->compose_ms = now_ms;
		}
	} else if (keysym != XKB_KEY_NoSymbol) {
		send_phone_key(terminal, keysym, TSM_VTE_INVALID, 0);
	}
	terminal->cursor_visible = true;
	terminal->cursor_ms = now_ms;
	terminal->dirty = true;
}

static bool composition_paused(const struct fplinux_terminal *terminal)
{
	return terminal->star_down || terminal->softleft_down ||
	       terminal->modifier_panel ||
	       terminal->menu != FPLINUX_TERMINAL_MENU_CLOSED;
}

void fplinux_terminal_tick(struct fplinux_terminal *terminal, uint64_t now_ms,
			   bool shell_foreground)
{
	if (!terminal->active)
		return;
	resume_composition(terminal, now_ms);
	if (terminal->star_down && !terminal->star_held &&
	    now_ms - terminal->star_ms >= FPLINUX_TERMINAL_STAR_HOLD_MS)
		hold_star(terminal, now_ms);
	if (terminal->softleft_down && !terminal->softleft_held &&
	    !terminal->modifier_panel &&
	    now_ms - terminal->softleft_ms >=
		    FPLINUX_TERMINAL_SOFTLEFT_HOLD_MS) {
		open_modifiers(terminal);
		terminal->softleft_held = true;
	}
	if (terminal->pound_down && !terminal->pound_held &&
	    now_ms - terminal->pound_ms >= FPLINUX_TERMINAL_POUND_HOLD_MS)
		clear_line(terminal, shell_foreground);
	if (!composition_paused(terminal) &&
	    fplinux_multitap_pending(&terminal->compose) &&
	    now_ms - terminal->compose_ms >= FPLINUX_MULTITAP_TIMEOUT_MS)
		commit_composition(terminal);
	if (!terminal->history && !composition_paused(terminal) &&
	    !fplinux_multitap_pending(&terminal->compose) &&
	    now_ms - terminal->cursor_ms >= FPLINUX_TERMINAL_CURSOR_MS &&
	    !(tsm_screen_get_flags(terminal->screen) &
	      TSM_SCREEN_HIDE_CURSOR)) {
		terminal->cursor_visible = !terminal->cursor_visible;
		terminal->cursor_ms = now_ms;
		terminal->dirty = true;
	}
}

static int deadline_timeout(uint64_t now_ms, uint64_t start_ms,
			    unsigned int duration_ms, int current)
{
	uint64_t elapsed = now_ms - start_ms;
	int timeout = elapsed >= duration_ms ? 0 : (int)(duration_ms - elapsed);

	return current < 0 || timeout < current ? timeout : current;
}

int fplinux_terminal_timeout(const struct fplinux_terminal *terminal,
			     uint64_t now_ms)
{
	int timeout = -1;

	if (!terminal->active)
		return -1;
	if (terminal->compose_resume_pending)
		return 0;
	if (terminal->star_down && !terminal->star_held)
		timeout = deadline_timeout(now_ms, terminal->star_ms,
					   FPLINUX_TERMINAL_STAR_HOLD_MS,
					   timeout);
	if (terminal->softleft_down && !terminal->softleft_held &&
	    !terminal->modifier_panel)
		timeout = deadline_timeout(now_ms, terminal->softleft_ms,
					   FPLINUX_TERMINAL_SOFTLEFT_HOLD_MS,
					   timeout);
	if (terminal->pound_down && !terminal->pound_held)
		timeout = deadline_timeout(now_ms, terminal->pound_ms,
					   FPLINUX_TERMINAL_POUND_HOLD_MS,
					   timeout);
	if (!composition_paused(terminal) &&
	    fplinux_multitap_pending(&terminal->compose))
		timeout = deadline_timeout(now_ms, terminal->compose_ms,
					   FPLINUX_MULTITAP_TIMEOUT_MS,
					   timeout);
	if (!terminal->history && !composition_paused(terminal) &&
	    !fplinux_multitap_pending(&terminal->compose) &&
	    !(tsm_screen_get_flags(terminal->screen) & TSM_SCREEN_HIDE_CURSOR))
		timeout = deadline_timeout(now_ms, terminal->cursor_ms,
					   FPLINUX_TERMINAL_CURSOR_MS, timeout);
	return timeout;
}

void fplinux_terminal_reset_input(struct fplinux_terminal *terminal)
{
	fplinux_multitap_cancel(&terminal->compose);
	terminal->modifier = 0;
	terminal->modifier_selection = 0;
	terminal->modifier_panel = false;
	terminal->compose_resume_pending = false;
	terminal->star_down = false;
	terminal->star_held = false;
	terminal->softleft_down = false;
	terminal->softleft_held = false;
	terminal->pound_down = false;
	terminal->pound_held = false;
	terminal->menu = FPLINUX_TERMINAL_MENU_CLOSED;
	terminal->help_scroll = 0;
	terminal->dirty = true;
}

void fplinux_terminal_focus(struct fplinux_terminal *terminal, bool active,
			    uint64_t now_ms)
{
	fplinux_terminal_reset_input(terminal);
	terminal->active = active;
	terminal->cursor_visible = active;
	terminal->cursor_ms = now_ms;
	terminal->dirty = active;
}

uint32_t fplinux_terminal_preedit(const struct fplinux_terminal *terminal)
{
	uint32_t candidate = fplinux_multitap_candidate(&terminal->compose);

	return terminal->alphabetic_mode == FPLINUX_TERMINAL_UPPER ||
			       (terminal->modifier & TSM_SHIFT_MASK) ?
		       uppercase(candidate) :
		       candidate;
}

unsigned int
fplinux_terminal_menu_count(const struct fplinux_terminal *terminal)
{
	switch (terminal->menu) {
	case FPLINUX_TERMINAL_MENU_MAIN:
		return sizeof(main_menu) / sizeof(main_menu[0]);
	case FPLINUX_TERMINAL_MENU_SYMBOLS:
		return sizeof(symbols) / sizeof(symbols[0]);
	case FPLINUX_TERMINAL_MENU_SPECIAL:
		return sizeof(special_keys) / sizeof(special_keys[0]);
	case FPLINUX_TERMINAL_MENU_LANGUAGE:
		return sizeof(languages) / sizeof(languages[0]);
	default:
		return 0;
	}
}

const char *fplinux_terminal_menu_label(const struct fplinux_terminal *terminal,
					unsigned int index)
{
	if (index >= fplinux_terminal_menu_count(terminal))
		return "";
	if (terminal->menu == FPLINUX_TERMINAL_MENU_SYMBOLS)
		return symbols[index];
	if (terminal->menu == FPLINUX_TERMINAL_MENU_SPECIAL)
		return special_keys[index];
	if (terminal->menu == FPLINUX_TERMINAL_MENU_LANGUAGE)
		return languages[index];
	if (index == MENU_LANGUAGE &&
	    terminal->compose.language == FPLINUX_MULTITAP_RUSSIAN)
		return "Language: RU...";
	return main_menu[index];
}

void fplinux_terminal_status(const struct fplinux_terminal *terminal,
			     char *text, size_t size)
{
	const char *left = "Menu";
	const char *right = "History";
	const char *mode = terminal->mode == FPLINUX_TERMINAL_NUMERIC ? "123" :
			   terminal->mode == FPLINUX_TERMINAL_UPPER   ? "ABC" :
									"abc";
	const char *language =
		terminal->compose.language == FPLINUX_MULTITAP_RUSSIAN ? "RU" :
									 "EN";
	char middle[32];
	size_t columns = tsm_screen_get_width(terminal->screen);
	size_t middle_size;

	if (!size)
		return;
	snprintf(middle, sizeof(middle), "%s %s", mode, language);
	if (terminal->modifier)
		right = "Clear";
	if (terminal->history) {
		snprintf(middle, sizeof(middle), "-%uln",
			 fplinux_terminal_history_offset(terminal));
		right = "Back";
	}
	if (terminal->menu != FPLINUX_TERMINAL_MENU_CLOSED) {
		left = "Select";
		right = "Back";
		middle[0] = '\0';
	}
	if (terminal->menu == FPLINUX_TERMINAL_MENU_HELP) {
		struct fplinux_terminal_help help;

		left = "";
		fplinux_terminal_help_open(terminal, &help);
		if (help.max_offset)
			snprintf(middle, sizeof(middle), "%u-%u/%u",
				 help.offset + 1, help.offset + help.body_rows,
				 help.line_count);
	}
	if (terminal->modifier_panel) {
		left = "Done";
		right = "Cancel";
	}
	if (columns >= size)
		columns = size - 1;
	memset(text, ' ', columns);
	text[columns] = '\0';
	if (columns < strlen(left) + strlen(right) + 2)
		return;
	memcpy(text, left, strlen(left));
	memcpy(text + columns - strlen(right), right, strlen(right));
	middle_size = strlen(middle);
	if (middle_size <= columns - strlen(left) - strlen(right) - 2)
		memcpy(text + strlen(left) + 1, middle, middle_size);
}
