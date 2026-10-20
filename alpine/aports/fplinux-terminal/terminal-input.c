/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"

#include <ctype.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define FPLINUX_TERMINAL_POUND_HOLD_MS 650U
#define FPLINUX_TERMINAL_STAR_HOLD_MS 650U
#define FPLINUX_TERMINAL_CURSOR_MS 600U

#define FPLINUX_TERMINAL_TITLE_BYTES 128U

const unsigned int fplinux_terminal_modifier_masks[] = {
	TSM_CONTROL_MASK,
	TSM_ALT_MASK,
	TSM_SHIFT_MASK,
};
/* Key cap names, as printed on keyboards in every language. */
const char *const fplinux_terminal_modifier_names[] = { "Ctrl", "Alt",
							"Shift" };

/* Labels translated when shown; NULL takes the system word of the type. */
static const struct {
	enum fplinux_terminal_command_type type;
	const char *label;
} commands[] = {
	[FPLINUX_TERMINAL_COMMAND_ENTER] = { FPLINUX_TERMINAL_TYPE_OK,
					     N_("Enter") },
	[FPLINUX_TERMINAL_COMMAND_CLEAR] = { FPLINUX_TERMINAL_TYPE_CANCEL,
					     N_("Clear") },
	[FPLINUX_TERMINAL_COMMAND_LIVE] = { FPLINUX_TERMINAL_TYPE_BACK, NULL },
	[FPLINUX_TERMINAL_COMMAND_MODIFIERS] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("Modifiers") },
	[FPLINUX_TERMINAL_COMMAND_SYMBOLS] = { FPLINUX_TERMINAL_TYPE_SCREEN,
					       N_("Symbols") },
	[FPLINUX_TERMINAL_COMMAND_SPECIAL] = { FPLINUX_TERMINAL_TYPE_SCREEN,
					       N_("Special keys") },
	/* Named by language_item. */
	[FPLINUX_TERMINAL_COMMAND_LANGUAGE] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						NULL },
	[FPLINUX_TERMINAL_COMMAND_SCROLLBACK] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						  N_("Scrollback") },
	[FPLINUX_TERMINAL_COMMAND_INTERRUPT] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("Interrupt Ctrl+C") },
	[FPLINUX_TERMINAL_COMMAND_END_INPUT] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("End input Ctrl+D") },
	[FPLINUX_TERMINAL_COMMAND_SEARCH] = { FPLINUX_TERMINAL_TYPE_SCREEN,
					      N_("Search Ctrl+R") },
	[FPLINUX_TERMINAL_COMMAND_HELP] = { FPLINUX_TERMINAL_TYPE_HELP, NULL },
	[FPLINUX_TERMINAL_COMMAND_EXIT] = { FPLINUX_TERMINAL_TYPE_EXIT, NULL },
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

/* The options menu shows the code in capitals. */
static void language_code(const struct fplinux_terminal *terminal,
			  char code[FPLINUX_LANGUAGE_CODE_BYTES])
{
	const char *source =
		terminal->languages.language[terminal->language].code;
	size_t index;

	for (index = 0;
	     index + 1 < FPLINUX_LANGUAGE_CODE_BYTES && source[index]; ++index)
		code[index] = (char)toupper((unsigned char)source[index]);
	code[index] = '\0';
}

static void select_language(struct fplinux_terminal *terminal, size_t index)
{
	char code[FPLINUX_LANGUAGE_CODE_BYTES];

	terminal->language = index;
	fplinux_multitap_set_language(&terminal->compose,
				      &terminal->languages.language[index]);
	language_code(terminal, code);
	snprintf(terminal->language_item, sizeof(terminal->language_item),
		 _("Language: %s"), code);
	terminal->dirty = true;
}

void fplinux_terminal_load_languages(struct fplinux_terminal *terminal,
				     const char *directory, const char *first)
{
	fplinux_multitap_load_languages(&terminal->languages, directory, first);
	select_language(terminal, 0);
}

static void send_character(struct fplinux_terminal *terminal,
			   uint32_t character)
{
	uint32_t ascii = character < 128 ? character : TSM_VTE_INVALID;
	uint32_t keysym;

	if (terminal->alphabetic_mode == FPLINUX_TERMINAL_UPPER ||
	    (terminal->modifier & TSM_SHIFT_MASK))
		character = fplinux_multitap_uppercase(character);
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

void fplinux_terminal_key(struct fplinux_terminal *terminal, uint32_t keysym,
			  uint32_t ascii, unsigned int modifiers,
			  uint32_t unicode)
{
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

	if (code == KEY_NUMERIC_STAR) {
		if (repeat)
			return;
		if (pressed) {
			return_live(terminal);
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
		}
		terminal->dirty = true;
		return;
	}
	if (!pressed)
		return;
	if (terminal->history &&
	    (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down ||
	     keysym == XKB_KEY_Left || keysym == XKB_KEY_Right)) {
		scroll(terminal, keysym);
		return;
	}
	return_live(terminal);
	if (code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9) {
		unsigned char character =
			(unsigned char)('0' + code - KEY_NUMERIC_0);
		/* A key queued before the terminal was uncovered is older. */
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
	return terminal->star_down || terminal->paused;
}

void fplinux_terminal_cancel_held(struct fplinux_terminal *terminal)
{
	terminal->star_down = false;
	terminal->star_held = false;
	terminal->pound_down = false;
	terminal->pound_held = false;
	terminal->pound_cancelled = false;
	terminal->dirty = true;
}

void fplinux_terminal_pause(struct fplinux_terminal *terminal, bool paused,
			    uint64_t now_ms)
{
	if (terminal->paused == paused)
		return;
	terminal->paused = paused;
	/* A pending letter waited under the cover; it gets a fresh timeout. */
	if (!paused)
		terminal->compose_ms = now_ms;
	/* A held * or # is let go under the cover; its gesture does nothing. */
	if (paused)
		fplinux_terminal_cancel_held(terminal);
	terminal->dirty = true;
}

void fplinux_terminal_tick(struct fplinux_terminal *terminal, uint64_t now_ms,
			   bool shell_foreground)
{
	if (terminal->star_down && !terminal->star_held &&
	    now_ms - terminal->star_ms >= FPLINUX_TERMINAL_STAR_HOLD_MS)
		hold_star(terminal, now_ms);
	if (terminal->pound_down && !terminal->pound_held &&
	    now_ms - terminal->pound_ms >= FPLINUX_TERMINAL_POUND_HOLD_MS)
		clear_line(terminal, shell_foreground);
	if (!composition_paused(terminal) &&
	    fplinux_multitap_pending(&terminal->compose) &&
	    now_ms - terminal->compose_ms >= terminal->compose.timeout_ms)
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

uint32_t fplinux_terminal_preedit(const struct fplinux_terminal *terminal)
{
	uint32_t candidate = fplinux_multitap_candidate(&terminal->compose);

	return terminal->alphabetic_mode == FPLINUX_TERMINAL_UPPER ||
			       (terminal->modifier & TSM_SHIFT_MASK) ?
		       fplinux_multitap_uppercase(candidate) :
		       candidate;
}

bool fplinux_terminal_command(const struct fplinux_terminal *terminal,
			      enum fplinux_terminal_command command,
			      struct fplinux_terminal_command_info *info)
{
	if ((unsigned int)command >= FPLINUX_TERMINAL_COMMAND_COUNT)
		return false;
	/* The scrollback has Back for Clear and does not offer itself. */
	if (terminal->history ?
		    command == FPLINUX_TERMINAL_COMMAND_CLEAR ||
			    command == FPLINUX_TERMINAL_COMMAND_SCROLLBACK :
		    command == FPLINUX_TERMINAL_COMMAND_LIVE)
		return false;
	info->type = commands[command].type;
	info->label = command == FPLINUX_TERMINAL_COMMAND_LANGUAGE ?
			      terminal->language_item :
		      commands[command].label ? _(commands[command].label) :
						NULL;
	/* The options menu follows the order of the commands. */
	info->priority = (int)command;
	return true;
}

/* Cancels a pending letter, else disarms the modifiers, else erases. */
static void clear(struct fplinux_terminal *terminal)
{
	if (fplinux_multitap_pending(&terminal->compose))
		fplinux_multitap_cancel(&terminal->compose);
	else if (terminal->modifier)
		terminal->modifier = 0;
	else
		send_phone_key(terminal, XKB_KEY_BackSpace, TSM_VTE_INVALID, 0);
	terminal->dirty = true;
}

void fplinux_terminal_run(struct fplinux_terminal *terminal,
			  enum fplinux_terminal_command command)
{
	switch (command) {
	case FPLINUX_TERMINAL_COMMAND_ENTER:
		send_phone_key(terminal, XKB_KEY_Return, TSM_VTE_INVALID, 0);
		break;
	case FPLINUX_TERMINAL_COMMAND_CLEAR:
		clear(terminal);
		break;
	case FPLINUX_TERMINAL_COMMAND_LIVE:
		return_live(terminal);
		break;
	case FPLINUX_TERMINAL_COMMAND_SCROLLBACK:
		if (commit_composition(terminal))
			terminal->history = true;
		break;
	case FPLINUX_TERMINAL_COMMAND_INTERRUPT:
		send_phone_key(terminal, XKB_KEY_c, 'c', TSM_CONTROL_MASK);
		break;
	case FPLINUX_TERMINAL_COMMAND_END_INPUT:
		send_phone_key(terminal, XKB_KEY_d, 'd', TSM_CONTROL_MASK);
		break;
	case FPLINUX_TERMINAL_COMMAND_SEARCH:
		send_phone_key(terminal, XKB_KEY_r, 'r', TSM_CONTROL_MASK);
		break;
	default:
		return;
	}
	terminal->dirty = true;
}

void fplinux_terminal_set_modifiers(struct fplinux_terminal *terminal,
				    unsigned int modifiers)
{
	terminal->modifier = modifiers &
			     (TSM_CONTROL_MASK | TSM_ALT_MASK | TSM_SHIFT_MASK);
	terminal->dirty = true;
}

size_t fplinux_terminal_symbol_count(void)
{
	return sizeof(symbols) / sizeof(symbols[0]);
}

const char *fplinux_terminal_symbol(size_t index)
{
	return index < fplinux_terminal_symbol_count() ? symbols[index] : "";
}

void fplinux_terminal_type_symbol(struct fplinux_terminal *terminal,
				  size_t index)
{
	if (index >= fplinux_terminal_symbol_count())
		return;
	return_live(terminal);
	if (commit_composition(terminal))
		send_character(terminal, (unsigned char)symbols[index][0]);
}

size_t fplinux_terminal_special_count(void)
{
	return sizeof(special_keys) / sizeof(special_keys[0]);
}

const char *fplinux_terminal_special(size_t index)
{
	return index < fplinux_terminal_special_count() ? special_keys[index] :
							  "";
}

void fplinux_terminal_send_special(struct fplinux_terminal *terminal,
				   size_t index)
{
	if (index >= fplinux_terminal_special_count())
		return;
	send_phone_key(terminal,
		       index ? XKB_KEY_F1 + (uint32_t)index - 1 :
			       XKB_KEY_Escape,
		       TSM_VTE_INVALID, 0);
	terminal->dirty = true;
}

void fplinux_terminal_select_language(struct fplinux_terminal *terminal,
				      size_t index)
{
	if (index >= terminal->languages.count || !commit_composition(terminal))
		return;
	select_language(terminal, index);
}

void fplinux_terminal_title(const struct fplinux_terminal *terminal,
			    fplinux_terminal_title_fits fits, void *context,
			    char *text, size_t size)
{
	const struct fplinux_multitap_language *language =
		&terminal->languages.language[terminal->language];
	const char *mode = terminal->mode == FPLINUX_TERMINAL_NUMERIC ?
				   "123" :
			   terminal->alphabetic_mode == FPLINUX_TERMINAL_UPPER ?
				   language->indicator[FPLINUX_LANGUAGE_UPPER] :
				   language->indicator[FPLINUX_LANGUAGE_LOWER];
	char forms[3][FPLINUX_TERMINAL_TITLE_BYTES];
	char modifiers[32] = "";
	size_t count = 0;
	size_t chosen = 0;
	size_t used = 0;

	if (!size)
		return;
	if (terminal->history) {
		unsigned int lines = fplinux_terminal_history_offset(terminal);

		/* The scrollback's one form; the band shortens it if needed. */
		snprintf(forms[count++], sizeof(forms[0]),
			 ngettext("%u line above", "%u lines above", lines),
			 lines);
	} else {
		for (unsigned int index = 0;
		     index < FPLINUX_TERMINAL_MODIFIER_COUNT; ++index)
			if (terminal->modifier &
			    fplinux_terminal_modifier_masks[index])
				used += (size_t)snprintf(
					modifiers + used,
					sizeof(modifiers) - used,
					used ? "+%s" : "%s",
					fplinux_terminal_modifier_names[index]);
		/* A status line, not a sentence: two spaces part its fields. */
		if (*modifiers) {
			snprintf(forms[count++], sizeof(forms[0]), "%s  %s  %s",
				 _("Terminal"), modifiers, mode);
			snprintf(forms[count++], sizeof(forms[0]), "%s  %s",
				 modifiers, mode);
		} else {
			snprintf(forms[count++], sizeof(forms[0]), "%s  %s",
				 _("Terminal"), mode);
		}
		snprintf(forms[count++], sizeof(forms[0]), "%s", mode);
	}
	/* The first form that fits; the last is the shortest and stays. */
	while (chosen + 1 < count && !fits(forms[chosen], context))
		++chosen;
	snprintf(text, size, "%s", forms[chosen]);
}
