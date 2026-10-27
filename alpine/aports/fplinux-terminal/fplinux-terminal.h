/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_H
#define FPLINUX_TERMINAL_H

#include <libintl.h>
#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fplinux-text-input.h"

/*
 * Visible text is English and translated through the application's gettext
 * domain; N_() marks text that is translated where it is shown.
 */
#define _(text) gettext(text)
#define N_(text) text

#define FPLINUX_TERMINAL_SCROLLBACK_LINES 512U
#define FPLINUX_TERMINAL_OUTPUT_BYTES 16384U

/*
 * The commands of the terminal screen. The GUI attaches them to its canvas
 * and the runtime gives them their keys. While the shell runs, OK is Enter
 * and the right soft key is Clear in the live view or Back in scrollback.
 * After an unsuccessful shell exit, Close replaces Clear. The left soft key
 * opens the options menu with the rest in this order.
 */
enum fplinux_terminal_command {
	FPLINUX_TERMINAL_COMMAND_ENTER,
	FPLINUX_TERMINAL_COMMAND_CLEAR,
	FPLINUX_TERMINAL_COMMAND_LIVE,
	FPLINUX_TERMINAL_COMMAND_MODIFIERS,
	FPLINUX_TERMINAL_COMMAND_SYMBOLS,
	FPLINUX_TERMINAL_COMMAND_SPECIAL,
	FPLINUX_TERMINAL_COMMAND_SCROLLBACK,
	FPLINUX_TERMINAL_COMMAND_INTERRUPT,
	FPLINUX_TERMINAL_COMMAND_END_INPUT,
	FPLINUX_TERMINAL_COMMAND_SEARCH,
	FPLINUX_TERMINAL_COMMAND_FONT_SIZE,
	FPLINUX_TERMINAL_COMMAND_HELP,
	FPLINUX_TERMINAL_COMMAND_EXIT,
	FPLINUX_TERMINAL_COMMAND_INSERT,
	FPLINUX_TERMINAL_COMMAND_CANCEL,
	FPLINUX_TERMINAL_COMMAND_COUNT,
};

/*
 * The values of enum fplinux_ui_command_type that the commands use; that
 * header needs LVGL, so the engine keeps the numbers and the GUI checks them.
 */
enum fplinux_terminal_command_type {
	FPLINUX_TERMINAL_TYPE_BACK = 0,
	FPLINUX_TERMINAL_TYPE_CANCEL = 1,
	FPLINUX_TERMINAL_TYPE_EXIT = 2,
	FPLINUX_TERMINAL_TYPE_OK = 4,
	FPLINUX_TERMINAL_TYPE_SCREEN = 6,
	FPLINUX_TERMINAL_TYPE_HELP = 7,
};

struct fplinux_terminal_command_info {
	enum fplinux_terminal_command_type type;
	/* Translated, or NULL for the system word of the type. */
	const char *label;
	int priority;
};

struct fplinux_terminal {
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	char output[FPLINUX_TERMINAL_OUTPUT_BYTES];
	size_t output_size;
	bool failed;
	bool dirty;
	bool history;
	bool cursor_visible;
	/* An unsuccessful shell exit leaves a read-only screen and scrollback. */
	bool stopped;
	/* Shared keypad modes, composition, physical holds and symbols. */
	struct fplinux_text_input input;
	/* Hidden, or covered by another screen or the options menu. */
	bool paused;
	unsigned int modifier;
	uint64_t cursor_ms;
};

bool fplinux_terminal_init(struct fplinux_terminal *terminal,
			   unsigned int columns, unsigned int rows);
void fplinux_terminal_destroy(struct fplinux_terminal *terminal);
/*
 * Replaces the built-in English input language with the input languages of
 * the language files in directory and selects the first.
 */
void fplinux_terminal_load_languages(struct fplinux_terminal *terminal,
				     const char *directory, const char *first);
void fplinux_terminal_feed(struct fplinux_terminal *terminal, const char *bytes,
			   size_t size);
/* Remove only bytes actually accepted by the PTY; unsent input stays queued. */
void fplinux_terminal_consume(struct fplinux_terminal *terminal, size_t size);
/* False leaves a blocked event with its caller for a later retry. */
bool fplinux_terminal_key(struct fplinux_terminal *terminal, uint32_t keysym,
			  uint32_t ascii, unsigned int modifiers,
			  uint32_t unicode);
/* Returns false only when output is blocked and the event must be retried. */
bool fplinux_terminal_phone(struct fplinux_terminal *terminal,
			    unsigned int code, bool pressed, uint64_t now_ms);
void fplinux_terminal_tick(struct fplinux_terminal *terminal, uint64_t now_ms,
			   bool can_input);
/* Backpressure suspends input deadlines until the PTY can accept bytes. */
bool fplinux_terminal_deadline(const struct fplinux_terminal *terminal,
			       bool can_input, uint64_t *deadline_ms);
/*
 * A pending letter waits while the terminal is hidden or covered by another
 * screen or the options menu and gets a fresh timeout when it is shown and
 * uncovered. Physical holds and an open symbol picker are cancelled.
 */
void fplinux_terminal_pause(struct fplinux_terminal *terminal, bool paused,
			    uint64_t now_ms);
/* Cancel physical holds while retaining composition, modifiers and PTY bytes. */
void fplinux_terminal_cancel_held(struct fplinux_terminal *terminal);
uint32_t fplinux_terminal_preedit(const struct fplinux_terminal *terminal);
unsigned int
fplinux_terminal_history_offset(const struct fplinux_terminal *terminal);

/* Whether the command belongs on the screen now, and how it is shown. */
bool fplinux_terminal_command(const struct fplinux_terminal *terminal,
			      enum fplinux_terminal_command command,
			      struct fplinux_terminal_command_info *info);
/*
 * Runs terminal actions and shared symbol selection. Other screens belong
 * to the GUI. False preserves a blocked command for a later retry.
 */
bool fplinux_terminal_run(struct fplinux_terminal *terminal,
			  enum fplinux_terminal_command command);
/* Ctrl, Alt and Shift as TSM masks, armed for the next phone key. */
#define FPLINUX_TERMINAL_MODIFIER_COUNT 3U
extern const unsigned int
	fplinux_terminal_modifier_masks[FPLINUX_TERMINAL_MODIFIER_COUNT];
extern const char *const
	fplinux_terminal_modifier_names[FPLINUX_TERMINAL_MODIFIER_COUNT];
void fplinux_terminal_set_modifiers(struct fplinux_terminal *terminal,
				    unsigned int modifiers);
size_t fplinux_terminal_special_count(void);
const char *fplinux_terminal_special(size_t index);
bool fplinux_terminal_send_special(struct fplinux_terminal *terminal,
				   size_t index);

#endif
