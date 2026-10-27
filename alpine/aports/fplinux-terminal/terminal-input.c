/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define FPLINUX_TERMINAL_CURSOR_MS 500U

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
	[FPLINUX_TERMINAL_COMMAND_SCROLLBACK] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						  N_("Scrollback") },
	[FPLINUX_TERMINAL_COMMAND_INTERRUPT] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("Interrupt Ctrl+C") },
	[FPLINUX_TERMINAL_COMMAND_END_INPUT] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("End input Ctrl+D") },
	[FPLINUX_TERMINAL_COMMAND_SEARCH] = { FPLINUX_TERMINAL_TYPE_SCREEN,
					      N_("Search Ctrl+R") },
	[FPLINUX_TERMINAL_COMMAND_FONT_SIZE] = { FPLINUX_TERMINAL_TYPE_SCREEN,
						 N_("Font size") },
	[FPLINUX_TERMINAL_COMMAND_HELP] = { FPLINUX_TERMINAL_TYPE_HELP, NULL },
	[FPLINUX_TERMINAL_COMMAND_EXIT] = { FPLINUX_TERMINAL_TYPE_EXIT, NULL },
	[FPLINUX_TERMINAL_COMMAND_INSERT] = { FPLINUX_TERMINAL_TYPE_OK,
					      N_("Insert") },
	[FPLINUX_TERMINAL_COMMAND_CANCEL] = { FPLINUX_TERMINAL_TYPE_CANCEL,
					      N_("Cancel") },
};
static const struct {
	const char *label;
	uint32_t keysym;
} special_keys[] = {
	{ "Esc", XKB_KEY_Escape },     { "Home", XKB_KEY_Home },
	{ "End", XKB_KEY_End },	       { "Delete", XKB_KEY_Delete },
	{ "PageUp", XKB_KEY_Page_Up }, { "PageDown", XKB_KEY_Page_Down },
	{ "F1", XKB_KEY_F1 },	       { "F2", XKB_KEY_F2 },
	{ "F3", XKB_KEY_F3 },	       { "F4", XKB_KEY_F4 },
	{ "F5", XKB_KEY_F5 },	       { "F6", XKB_KEY_F6 },
	{ "F7", XKB_KEY_F7 },	       { "F8", XKB_KEY_F8 },
	{ "F9", XKB_KEY_F9 },	       { "F10", XKB_KEY_F10 },
	{ "F11", XKB_KEY_F11 },	       { "F12", XKB_KEY_F12 },
};

void fplinux_terminal_load_languages(struct fplinux_terminal *terminal,
				     const char *directory, const char *first)
{
	fplinux_text_input_load_languages(&terminal->input, directory, first);
	terminal->dirty = true;
}

static bool can_send(const struct fplinux_terminal *terminal)
{
	/* Reserve room for one encoded key before asking the VTE to emit it. */
	return terminal->output_size <= sizeof(terminal->output) - 32;
}

static enum fplinux_multitap_emit_result send_character(void *context,
							uint32_t character)
{
	struct fplinux_terminal *terminal = context;
	uint32_t ascii;
	uint32_t keysym;

	if (!can_send(terminal))
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	character = fplinux_text_input_apply_case(&terminal->input, character,
						  false);
	if (terminal->modifier & TSM_SHIFT_MASK)
		character = fplinux_multitap_uppercase(character);
	ascii = character < 128 ? character : TSM_VTE_INVALID;
	keysym = character <= 0xff ? character : 0x01000000U | character;
	tsm_vte_handle_keyboard(terminal->vte, keysym, ascii,
				terminal->modifier, character);
	terminal->modifier = 0;
	terminal->dirty = true;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static bool commit_composition(struct fplinux_terminal *terminal)
{
	return fplinux_text_input_commit(&terminal->input, send_character,
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

static bool send_phone_key(struct fplinux_terminal *terminal, uint32_t keysym,
			   uint32_t character, unsigned int fixed_modifiers)
{
	unsigned int modifiers;

	if (!commit_composition(terminal) || !can_send(terminal))
		return false;
	return_live(terminal);
	/* Named shortcuts supply their complete combination after pending text. */
	modifiers = fixed_modifiers ? fixed_modifiers : terminal->modifier;
	if (keysym == XKB_KEY_Tab && (modifiers & TSM_SHIFT_MASK))
		keysym = XKB_KEY_ISO_Left_Tab;
	tsm_vte_handle_keyboard(terminal->vte, keysym, character, modifiers,
				character);
	terminal->modifier = 0;
	terminal->cursor_visible = true;
	return true;
}

/* Cancels a pending letter, else disarms the modifiers, else erases. */
static enum fplinux_multitap_emit_result erase_character(void *context)
{
	struct fplinux_terminal *terminal = context;

	if (fplinux_multitap_pending(&terminal->input.compose))
		fplinux_text_input_cancel(&terminal->input);
	else if (terminal->modifier)
		terminal->modifier = 0;
	else if (!send_phone_key(terminal, XKB_KEY_BackSpace, TSM_VTE_INVALID,
				 0))
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	terminal->dirty = true;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static bool picker_key(struct fplinux_terminal *terminal, unsigned int code);

bool fplinux_terminal_key(struct fplinux_terminal *terminal, uint32_t keysym,
			  uint32_t ascii, unsigned int modifiers,
			  uint32_t unicode)
{
	if (terminal->stopped) {
		if ((modifiers & ~TSM_LOCK_MASK) == TSM_SHIFT_MASK &&
		    (keysym == XKB_KEY_Page_Up || keysym == XKB_KEY_Page_Down))
			scroll(terminal, keysym);
		else if (terminal->history &&
			 (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down ||
			  keysym == XKB_KEY_Left || keysym == XKB_KEY_Right))
			scroll(terminal, keysym);
		return true;
	}
	if (terminal->input.picker_open) {
		switch (keysym) {
		case XKB_KEY_Left:
			return picker_key(terminal, KEY_LEFT);
		case XKB_KEY_Right:
			return picker_key(terminal, KEY_RIGHT);
		case XKB_KEY_Up:
			return picker_key(terminal, KEY_UP);
		case XKB_KEY_Down:
			return picker_key(terminal, KEY_DOWN);
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			return picker_key(terminal, KEY_OK);
		case XKB_KEY_Escape:
			return picker_key(terminal, KEY_F14);
		default:
			return true;
		}
	}
	if ((modifiers & ~TSM_LOCK_MASK) == TSM_SHIFT_MASK &&
	    (keysym == XKB_KEY_Page_Up || keysym == XKB_KEY_Page_Down)) {
		if (!commit_composition(terminal))
			return false;
		scroll(terminal, keysym);
		return true;
	}
	fplinux_text_input_cancel_held(&terminal->input);
	if (!commit_composition(terminal) || !can_send(terminal))
		return false;
	return_live(terminal);
	/* Phone one-shot modifiers never alter the external keyboard source. */
	tsm_vte_handle_keyboard(terminal->vte, keysym, ascii, modifiers,
				unicode);
	terminal->cursor_visible = true;
	return true;
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

static bool picker_key(struct fplinux_terminal *terminal, unsigned int code)
{
	enum fplinux_text_input_direction direction;

	switch (code) {
	case KEY_LEFT:
		direction = FPLINUX_TEXT_INPUT_LEFT;
		break;
	case KEY_RIGHT:
		direction = FPLINUX_TEXT_INPUT_RIGHT;
		break;
	case KEY_UP:
		direction = FPLINUX_TEXT_INPUT_UP;
		break;
	case KEY_DOWN:
		direction = FPLINUX_TEXT_INPUT_DOWN;
		break;
	case KEY_OK:
		if (fplinux_text_input_select_symbol(
			    &terminal->input, send_character, terminal) ==
		    FPLINUX_MULTITAP_EMIT_BLOCKED)
			return false;
		terminal->dirty = true;
		return true;
	case KEY_F14:
	case KEY_NUMERIC_STAR:
		fplinux_text_input_close_picker(&terminal->input);
		terminal->dirty = true;
		return true;
	default:
		return true;
	}
	fplinux_text_input_move_picker(&terminal->input, direction);
	terminal->dirty = true;
	return true;
}

bool fplinux_terminal_phone(struct fplinux_terminal *terminal,
			    unsigned int code, bool pressed, uint64_t now_ms)
{
	uint32_t keysym = phone_keysym(code);
	unsigned char input_key =
		code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9 ?
			(unsigned char)('0' + code - KEY_NUMERIC_0) :
		code == KEY_NUMERIC_POUND ? '#' :
		code == KEY_NUMERIC_STAR  ? '*' :
					    0;

	if (terminal->stopped) {
		if (pressed && terminal->history) {
			if (code == KEY_F14)
				return_live(terminal);
			else if (keysym != XKB_KEY_NoSymbol)
				scroll(terminal, keysym);
		}
		return true;
	}
	if (!pressed) {
		if (input_key) {
			enum fplinux_multitap_result result =
				fplinux_text_input_release(&terminal->input,
							   input_key, now_ms,
							   send_character,
							   terminal);

			if (result == FPLINUX_MULTITAP_BLOCKED)
				return false;
			terminal->dirty = true;
		}
		if (code == KEY_F14)
			fplinux_text_input_clear_release(&terminal->input);
		return true;
	}
	if (terminal->input.picker_open)
		return picker_key(terminal, code);
	if (terminal->history && code == KEY_F14) {
		return_live(terminal);
		return true;
	}
	if (terminal->history &&
	    (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down ||
	     keysym == XKB_KEY_Left || keysym == XKB_KEY_Right)) {
		scroll(terminal, keysym);
		return true;
	}
	return_live(terminal);
	if (input_key) {
		if (fplinux_text_input_press(
			    &terminal->input, input_key, now_ms, send_character,
			    terminal) == FPLINUX_MULTITAP_BLOCKED)
			return false;
	} else if (code == KEY_F14) {
		if (fplinux_text_input_clear_press(&terminal->input, now_ms,
						   erase_character, terminal) ==
		    FPLINUX_MULTITAP_EMIT_BLOCKED)
			return false;
	} else if (keysym != XKB_KEY_NoSymbol) {
		fplinux_text_input_cancel_held(&terminal->input);
		if (!send_phone_key(terminal, keysym, TSM_VTE_INVALID, 0))
			return false;
	}
	terminal->cursor_visible = true;
	terminal->cursor_ms = now_ms;
	terminal->dirty = true;
	return true;
}

void fplinux_terminal_cancel_held(struct fplinux_terminal *terminal)
{
	fplinux_text_input_cancel_held(&terminal->input);
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
		terminal->input.compose_ms = now_ms;
	if (paused)
		fplinux_terminal_cancel_held(terminal);
	terminal->dirty = true;
}

void fplinux_terminal_tick(struct fplinux_terminal *terminal, uint64_t now_ms,
			   bool can_input)
{
	bool pending = fplinux_multitap_pending(&terminal->input.compose);
	const char *indicator = fplinux_text_input_indicator(&terminal->input);
	bool picker = terminal->input.picker_open;

	if (terminal->paused || terminal->stopped)
		return;
	if (can_input)
		fplinux_text_input_tick(&terminal->input, now_ms,
					send_character, erase_character,
					terminal);
	if (pending != fplinux_multitap_pending(&terminal->input.compose) ||
	    indicator != fplinux_text_input_indicator(&terminal->input) ||
	    picker != terminal->input.picker_open)
		terminal->dirty = true;
	if (!terminal->history && !terminal->input.picker_open &&
	    !fplinux_multitap_pending(&terminal->input.compose) &&
	    now_ms - terminal->cursor_ms >= FPLINUX_TERMINAL_CURSOR_MS &&
	    !(tsm_screen_get_flags(terminal->screen) &
	      TSM_SCREEN_HIDE_CURSOR)) {
		terminal->cursor_visible = !terminal->cursor_visible;
		terminal->cursor_ms = now_ms;
		terminal->dirty = true;
	}
}

bool fplinux_terminal_deadline(const struct fplinux_terminal *terminal,
			       bool can_input, uint64_t *deadline_ms)
{
	uint64_t deadline = UINT64_MAX;

	if (terminal->paused || terminal->stopped)
		return false;
	if (can_input)
		fplinux_text_input_deadline(&terminal->input, &deadline);
	if (!terminal->history && !terminal->input.picker_open &&
	    !fplinux_multitap_pending(&terminal->input.compose) &&
	    !(tsm_screen_get_flags(terminal->screen) &
	      TSM_SCREEN_HIDE_CURSOR) &&
	    terminal->cursor_ms + FPLINUX_TERMINAL_CURSOR_MS < deadline)
		deadline = terminal->cursor_ms + FPLINUX_TERMINAL_CURSOR_MS;
	if (deadline == UINT64_MAX)
		return false;
	*deadline_ms = deadline;
	return true;
}

uint32_t fplinux_terminal_preedit(const struct fplinux_terminal *terminal)
{
	uint32_t candidate = fplinux_text_input_apply_case(
		&terminal->input,
		fplinux_multitap_candidate(&terminal->input.compose), false);

	return terminal->modifier & TSM_SHIFT_MASK ?
		       fplinux_multitap_uppercase(candidate) :
		       candidate;
}

bool fplinux_terminal_command(const struct fplinux_terminal *terminal,
			      enum fplinux_terminal_command command,
			      struct fplinux_terminal_command_info *info)
{
	bool picker_command = command == FPLINUX_TERMINAL_COMMAND_INSERT ||
			      command == FPLINUX_TERMINAL_COMMAND_CANCEL;

	if ((unsigned int)command >= FPLINUX_TERMINAL_COMMAND_COUNT)
		return false;
	if (terminal->stopped && command != FPLINUX_TERMINAL_COMMAND_LIVE &&
	    command != FPLINUX_TERMINAL_COMMAND_SCROLLBACK &&
	    command != FPLINUX_TERMINAL_COMMAND_FONT_SIZE &&
	    command != FPLINUX_TERMINAL_COMMAND_HELP &&
	    command != FPLINUX_TERMINAL_COMMAND_EXIT)
		return false;
	if (terminal->input.picker_open != picker_command)
		return false;
	/* The scrollback has Back for Clear and does not offer itself. */
	if (terminal->history ?
		    command == FPLINUX_TERMINAL_COMMAND_CLEAR ||
			    command == FPLINUX_TERMINAL_COMMAND_SCROLLBACK :
		    command == FPLINUX_TERMINAL_COMMAND_LIVE)
		return false;
	info->type = commands[command].type;
	info->label = commands[command].label ? _(commands[command].label) :
						NULL;
	if (terminal->stopped && command == FPLINUX_TERMINAL_COMMAND_EXIT)
		info->label = _("Close");
	/* The options menu follows the order of the commands. */
	info->priority = (int)command;
	return true;
}

bool fplinux_terminal_run(struct fplinux_terminal *terminal,
			  enum fplinux_terminal_command command)
{
	if (terminal->stopped && command != FPLINUX_TERMINAL_COMMAND_LIVE &&
	    command != FPLINUX_TERMINAL_COMMAND_SCROLLBACK)
		return true;
	switch (command) {
	case FPLINUX_TERMINAL_COMMAND_ENTER:
		if (!send_phone_key(terminal, XKB_KEY_Return, TSM_VTE_INVALID,
				    0))
			return false;
		break;
	case FPLINUX_TERMINAL_COMMAND_CLEAR:
		if (erase_character(terminal) == FPLINUX_MULTITAP_EMIT_BLOCKED)
			return false;
		break;
	case FPLINUX_TERMINAL_COMMAND_SYMBOLS:
		if (fplinux_text_input_open_picker(&terminal->input,
						   send_character, terminal) ==
		    FPLINUX_MULTITAP_BLOCKED)
			return false;
		return_live(terminal);
		break;
	case FPLINUX_TERMINAL_COMMAND_INSERT:
		if (terminal->input.picker_open &&
		    fplinux_text_input_select_symbol(
			    &terminal->input, send_character, terminal) ==
			    FPLINUX_MULTITAP_EMIT_BLOCKED)
			return false;
		break;
	case FPLINUX_TERMINAL_COMMAND_CANCEL:
		fplinux_text_input_close_picker(&terminal->input);
		break;
	case FPLINUX_TERMINAL_COMMAND_LIVE:
		return_live(terminal);
		break;
	case FPLINUX_TERMINAL_COMMAND_SCROLLBACK:
		if (!commit_composition(terminal))
			return false;
		terminal->history = true;
		break;
	case FPLINUX_TERMINAL_COMMAND_INTERRUPT:
		if (!send_phone_key(terminal, XKB_KEY_c, 'c', TSM_CONTROL_MASK))
			return false;
		break;
	case FPLINUX_TERMINAL_COMMAND_END_INPUT:
		if (!send_phone_key(terminal, XKB_KEY_d, 'd', TSM_CONTROL_MASK))
			return false;
		break;
	case FPLINUX_TERMINAL_COMMAND_SEARCH:
		if (!send_phone_key(terminal, XKB_KEY_r, 'r', TSM_CONTROL_MASK))
			return false;
		break;
	default:
		return true;
	}
	terminal->dirty = true;
	return true;
}

void fplinux_terminal_set_modifiers(struct fplinux_terminal *terminal,
				    unsigned int modifiers)
{
	if (terminal->stopped)
		return;
	terminal->modifier = modifiers &
			     (TSM_CONTROL_MASK | TSM_ALT_MASK | TSM_SHIFT_MASK);
	terminal->dirty = true;
}

size_t fplinux_terminal_special_count(void)
{
	return sizeof(special_keys) / sizeof(special_keys[0]);
}

const char *fplinux_terminal_special(size_t index)
{
	return index < fplinux_terminal_special_count() ?
		       special_keys[index].label :
		       "";
}

bool fplinux_terminal_send_special(struct fplinux_terminal *terminal,
				   size_t index)
{
	if (terminal->stopped || index >= fplinux_terminal_special_count())
		return true;
	if (!send_phone_key(terminal, special_keys[index].keysym,
			    TSM_VTE_INVALID, 0))
		return false;
	terminal->dirty = true;
	return true;
}
