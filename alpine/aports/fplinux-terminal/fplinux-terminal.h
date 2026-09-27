/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_H
#define FPLINUX_TERMINAL_H

#include <libtsm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fplinux-multitap.h"

#define FPLINUX_TERMINAL_SCROLLBACK_LINES 512U
#define FPLINUX_TERMINAL_OUTPUT_BYTES 16384U
#define FPLINUX_TERMINAL_CLEAR_LINE "\033[99~"

enum fplinux_terminal_mode {
	FPLINUX_TERMINAL_LOWER,
	FPLINUX_TERMINAL_UPPER,
	FPLINUX_TERMINAL_NUMERIC,
};

enum fplinux_terminal_menu {
	FPLINUX_TERMINAL_MENU_CLOSED,
	FPLINUX_TERMINAL_MENU_MAIN,
	FPLINUX_TERMINAL_MENU_SYMBOLS,
	FPLINUX_TERMINAL_MENU_SPECIAL,
	FPLINUX_TERMINAL_MENU_LANGUAGE,
	FPLINUX_TERMINAL_MENU_HELP,
};

struct fplinux_terminal {
	struct tsm_screen *screen;
	struct tsm_vte *vte;
	char output[FPLINUX_TERMINAL_OUTPUT_BYTES];
	size_t output_size;
	bool failed;
	bool dirty;
	bool history;
	bool editing;
	bool active;
	bool cursor_visible;
	bool diagnostic_requested;
	char shell_marker[64];
	struct fplinux_multitap compose;
	bool pound_down;
	bool pound_held;
	bool pound_cancelled;
	bool star_down;
	bool star_held;
	bool softleft_down;
	bool softleft_held;
	bool modifier_panel;
	bool compose_resume_pending;
	unsigned int modifier;
	unsigned int modifier_selection;
	unsigned int modifier_index;
	unsigned int menu_index;
	unsigned int help_scroll;
	enum fplinux_terminal_mode mode;
	enum fplinux_terminal_mode alphabetic_mode;
	enum fplinux_terminal_menu menu;
	uint64_t compose_ms;
	uint64_t pound_ms;
	uint64_t star_ms;
	uint64_t softleft_ms;
	uint64_t cursor_ms;
};

bool fplinux_terminal_init(struct fplinux_terminal *terminal,
			   unsigned int columns, unsigned int rows,
			   const char *shell_token);
void fplinux_terminal_destroy(struct fplinux_terminal *terminal);
void fplinux_terminal_feed(struct fplinux_terminal *terminal, const char *bytes,
			   size_t size);
/* Remove only bytes actually accepted by the PTY; unsent input stays queued. */
void fplinux_terminal_consume(struct fplinux_terminal *terminal, size_t size);
void fplinux_terminal_key(struct fplinux_terminal *terminal, uint32_t keysym,
			  uint32_t ascii, unsigned int modifiers,
			  uint32_t unicode);
void fplinux_terminal_phone(struct fplinux_terminal *terminal,
			    unsigned int code, bool pressed, bool repeat,
			    uint64_t now_ms, bool shell_foreground);
void fplinux_terminal_tick(struct fplinux_terminal *terminal, uint64_t now_ms,
			   bool shell_foreground);
int fplinux_terminal_timeout(const struct fplinux_terminal *terminal,
			     uint64_t now_ms);
void fplinux_terminal_focus(struct fplinux_terminal *terminal, bool active,
			    uint64_t now_ms);
void fplinux_terminal_reset_input(struct fplinux_terminal *terminal);
uint32_t fplinux_terminal_preedit(const struct fplinux_terminal *terminal);
unsigned int
fplinux_terminal_history_offset(const struct fplinux_terminal *terminal);
const char *fplinux_terminal_menu_label(const struct fplinux_terminal *terminal,
					unsigned int index);
unsigned int
fplinux_terminal_menu_count(const struct fplinux_terminal *terminal);
void fplinux_terminal_status(const struct fplinux_terminal *terminal,
			     char *text, size_t size);

#endif
