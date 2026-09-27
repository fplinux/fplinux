/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-terminal.h"
#include "terminal-pty.h"
#include "terminal-render.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void feed(struct fplinux_terminal *terminal, const char *text)
{
	fplinux_terminal_feed(terminal, text, strlen(text));
}

static void output_is(struct fplinux_terminal *terminal, const char *bytes,
		      size_t size)
{
	if (terminal->output_size != size ||
	    memcmp(terminal->output, bytes, size)) {
		size_t index;

		fprintf(stderr, "expected %zu PTY bytes; received %zu:", size,
			terminal->output_size);
		for (index = 0; index < terminal->output_size; ++index)
			fprintf(stderr, " %02x",
				(unsigned char)terminal->output[index]);
		fputc('\n', stderr);
	}
	assert(terminal->output_size == size);
	assert(!memcmp(terminal->output, bytes, size));
	fplinux_terminal_consume(terminal, size);
}

static void press(struct fplinux_terminal *terminal, unsigned int code,
		  uint64_t time)
{
	fplinux_terminal_phone(terminal, code, true, false, time, true);
	fplinux_terminal_phone(terminal, code, false, false, time + 1, true);
}

static void hold(struct fplinux_terminal *terminal, unsigned int code,
		 uint64_t time, unsigned int duration_ms)
{
	fplinux_terminal_phone(terminal, code, true, false, time, true);
	fplinux_terminal_tick(terminal, time + duration_ms, true);
	fplinux_terminal_phone(terminal, code, false, false,
			       time + duration_ms + 1, true);
}

static void arm_modifiers(struct fplinux_terminal *terminal,
			  const char *shortcuts, uint64_t time)
{
	hold(terminal, KEY_F13, time, 500);
	assert(terminal->modifier_panel);
	for (; *shortcuts; ++shortcuts)
		press(terminal, KEY_NUMERIC_0 + *shortcuts - '0', time + 510);
	press(terminal, KEY_F13, time + 520);
	assert(!terminal->modifier_panel);
}

static void choose_menu(struct fplinux_terminal *terminal, const char *label,
			uint64_t time)
{
	unsigned int index;
	unsigned int count = fplinux_terminal_menu_count(terminal);

	for (index = 0; index < count; ++index) {
		if (!strcmp(fplinux_terminal_menu_label(terminal,
							terminal->menu_index),
			    label)) {
			press(terminal, KEY_F13, time);
			return;
		}
		press(terminal, KEY_DOWN, time);
	}
	assert(!"menu action not found");
}

static void focused_menu_is(const struct fplinux_terminal *terminal,
			    const char *label)
{
	assert(!strcmp(fplinux_terminal_menu_label(terminal,
						   terminal->menu_index),
		       label));
}

static void verify_grid_and_colors(void)
{
	struct fplinux_terminal terminal;
	const struct tsm_screen_cell *cells;

	assert(fplinux_terminal_init(&terminal, 4, 2, "test"));
	feed(&terminal, "abcdEF");
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 'a' && cells[3].ch == 'd');
	assert(cells[4].ch == 'E' && cells[5].ch == 'F');
	assert(tsm_screen_get_cursor_x(terminal.screen) == 2);
	assert(tsm_screen_get_cursor_y(terminal.screen) == 1);
	feed(&terminal, "\033[2J\033[H\033[31mR\033[0mя");
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 'R' && cells[1].ch == 0x44f);
	assert(cells[0].fg.r == 205 && cells[0].fg.g == 0);
	assert(cells[1].fg.r == 216 && cells[1].bg.r == 17);
	feed(&terminal, "\033[?1049h\033[Halt");
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 'a');
	feed(&terminal, "\033[?1049l");
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 'R' && cells[1].ch == 0x44f);
	fplinux_terminal_destroy(&terminal);
}

static void verify_composition_and_key_sequences(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	press(&terminal, KEY_NUMERIC_2, 10);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	assert(terminal.output_size == 0);
	fplinux_terminal_key(&terminal, XKB_KEY_Tab, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	output_is(&terminal, "a\t", 2);
	press(&terminal, KEY_NUMERIC_3, 20);
	press(&terminal, KEY_OK, 30);
	output_is(&terminal, "d\r", 2);
	arm_modifiers(&terminal, "1", 40);
	press(&terminal, KEY_NUMERIC_2, 600);
	press(&terminal, KEY_NUMERIC_2, 610);
	press(&terminal, KEY_NUMERIC_2, 620);
	press(&terminal, KEY_OK, 630);
	output_is(&terminal, "\003\r", 2);
	arm_modifiers(&terminal, "3", 640);
	press(&terminal, KEY_PICKUP_PHONE, 1200);
	output_is(&terminal, "\033[Z", 3);
	assert(terminal.modifier == 0);
	feed(&terminal, "\033[?1h");
	fplinux_terminal_key(&terminal, XKB_KEY_Up, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	output_is(&terminal, "\033OA", 3);
	fplinux_multitap_set_language(&terminal.compose,
				      FPLINUX_MULTITAP_RUSSIAN);
	press(&terminal, KEY_NUMERIC_2, 1300);
	press(&terminal, KEY_PICKUP_PHONE, 1320);
	output_is(&terminal, "а\t", 3);
	fplinux_terminal_destroy(&terminal);
}

static void verify_pound_and_focus(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 30, 19, "test"));
	press(&terminal, KEY_NUMERIC_2, 10);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true, false, 600,
			       true);
	fplinux_terminal_tick(&terminal, 710, true);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false, false, 720,
			       true);
	assert(!fplinux_terminal_preedit(&terminal));
	output_is(&terminal, "", 0);
	press(&terminal, KEY_NUMERIC_POUND, 30);
	output_is(&terminal, "\177", 1);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true, false, 100,
			       true);
	fplinux_terminal_tick(&terminal, 750, true);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false, false, 760,
			       true);
	output_is(&terminal, "", 0);
	feed(&terminal, "\033]777;test;B\a");
	assert(terminal.editing);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true, false, 1000,
			       true);
	fplinux_terminal_tick(&terminal, 1650, true);
	fplinux_terminal_tick(&terminal, 2300, true);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false, false, 2400,
			       true);
	output_is(&terminal, "\033[99~", 5);
	feed(&terminal, "\033]777;test;C\a");
	assert(!terminal.editing);
	arm_modifiers(&terminal, "12", 2500);
	press(&terminal, KEY_NUMERIC_2, 3100);
	hold(&terminal, KEY_F13, 3110, 500);
	press(&terminal, KEY_NUMERIC_3, 3620);
	fplinux_terminal_focus(&terminal, false, 3700);
	assert(!terminal.modifier && !fplinux_terminal_preedit(&terminal));
	assert(!terminal.modifier_panel && !terminal.modifier_selection);
	assert(fplinux_terminal_timeout(&terminal, 3800) == -1);
	feed(&terminal, "background");
	fplinux_terminal_tick(&terminal, 5000, true);
	assert(!terminal.cursor_visible);
	fplinux_terminal_focus(&terminal, true, 5100);
	assert(tsm_screen_draw2(terminal.screen)[0].ch == 'b');
	fplinux_terminal_destroy(&terminal);
}

static void verify_menu_softkey_selection(void)
{
	struct fplinux_terminal terminal;
	char status[64];

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	press(&terminal, KEY_F13, 10);
	fplinux_terminal_status(&terminal, status, sizeof(status));
	assert(!memcmp(status, "Select", 6));
	focused_menu_is(&terminal, "Modifiers");
	press(&terminal, KEY_UP, 20);
	focused_menu_is(&terminal, "Diagnostic console");
	press(&terminal, KEY_DOWN, 30);
	focused_menu_is(&terminal, "Modifiers");
	press(&terminal, KEY_DOWN, 40);
	press(&terminal, KEY_DOWN, 50);
	press(&terminal, KEY_DOWN, 60);
	focused_menu_is(&terminal, "Language: EN...");
	press(&terminal, KEY_DOWN, 65);
	focused_menu_is(&terminal, "Interrupt Ctrl+C");
	press(&terminal, KEY_UP, 66);
	focused_menu_is(&terminal, "Language: EN...");
	choose_menu(&terminal, "Search Ctrl+R", 67);
	output_is(&terminal, "\022", 1);
	press(&terminal, KEY_F13, 68);
	choose_menu(&terminal, "End input Ctrl+D", 69);
	output_is(&terminal, "\004", 1);
	press(&terminal, KEY_F13, 70);
	choose_menu(&terminal, "Interrupt Ctrl+C", 71);
	output_is(&terminal, "\003", 1);
	press(&terminal, KEY_NUMERIC_2, 80);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	press(&terminal, KEY_F13, 90);
	output_is(&terminal, "", 0);
	choose_menu(&terminal, "Symbols...", 160);
	press(&terminal, KEY_F13, 170);
	output_is(&terminal, "a#", 2);
	press(&terminal, KEY_F13, 180);
	press(&terminal, KEY_NUMERIC_POUND, 190);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED);
	output_is(&terminal, "\177", 1);
	press(&terminal, KEY_NUMERIC_2, 200);
	press(&terminal, KEY_F13, 210);
	press(&terminal, KEY_NUMERIC_POUND, 220);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED);
	assert(!fplinux_terminal_preedit(&terminal));
	output_is(&terminal, "", 0);
	fplinux_terminal_destroy(&terminal);
}

static void verify_menu_choosers_and_back(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	arm_modifiers(&terminal, "1", 10);
	press(&terminal, KEY_NUMERIC_2, 540);
	press(&terminal, KEY_F13, 550);
	choose_menu(&terminal, "Symbols...", 560);
	press(&terminal, KEY_F14, 570);
	focused_menu_is(&terminal, "Symbols...");
	choose_menu(&terminal, "Special keys...", 580);
	focused_menu_is(&terminal, "Esc");
	press(&terminal, KEY_F14, 590);
	focused_menu_is(&terminal, "Special keys...");
	choose_menu(&terminal, "Language: EN...", 600);
	focused_menu_is(&terminal, "English (EN)");
	press(&terminal, KEY_F14, 610);
	focused_menu_is(&terminal, "Language: EN...");
	fplinux_terminal_tick(&terminal, 2000, true);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	assert(terminal.modifier == TSM_CONTROL_MASK);
	press(&terminal, KEY_F14, 2010);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED);
	assert(terminal.modifier == TSM_CONTROL_MASK);
	output_is(&terminal, "", 0);
	press(&terminal, KEY_NUMERIC_POUND, 2020);
	press(&terminal, KEY_F13, 2030);
	choose_menu(&terminal, "Language: EN...", 2040);
	choose_menu(&terminal, "Russian (RU)", 2050);
	press(&terminal, KEY_NUMERIC_2, 2060);
	press(&terminal, KEY_OK, 2070);
	output_is(&terminal, "а\r", 3);
	press(&terminal, KEY_F13, 2080);
	choose_menu(&terminal, "Language: RU...", 2090);
	focused_menu_is(&terminal, "Russian (RU)");
	choose_menu(&terminal, "English (EN)", 2100);
	press(&terminal, KEY_F13, 2110);
	choose_menu(&terminal, "Special keys...", 2120);
	choose_menu(&terminal, "Esc", 2130);
	output_is(&terminal, "\033", 1);
	press(&terminal, KEY_F13, 2140);
	choose_menu(&terminal, "Special keys...", 2150);
	choose_menu(&terminal, "F1", 2160);
	output_is(&terminal, "\033OP", 3);
	press(&terminal, KEY_F13, 2170);
	choose_menu(&terminal, "Special keys...", 2180);
	choose_menu(&terminal, "F12", 2190);
	output_is(&terminal, "\033[24~", 5);
	fplinux_terminal_destroy(&terminal);
}

static void verify_help_preserves_input(void)
{
	static const unsigned int phone_keys[] = {
		KEY_F13,       KEY_NUMERIC_STAR, KEY_NUMERIC_POUND,
		KEY_NUMERIC_2, KEY_OK,		 KEY_PICKUP_PHONE,
		KEY_UP,	       KEY_DOWN,
	};
	static const uint32_t keyboard_keys[] = {
		XKB_KEY_x,   XKB_KEY_Return, XKB_KEY_BackSpace,
		XKB_KEY_F13, XKB_KEY_F14,    XKB_KEY_F1,
	};
	struct fplinux_terminal terminal;
	size_t index;
	uint64_t time = 600;
	char status[64];

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	arm_modifiers(&terminal, "12", 10);
	press(&terminal, KEY_NUMERIC_2, 540);
	press(&terminal, KEY_F13, 550);
	choose_menu(&terminal, "Help", 560);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_HELP);
	fplinux_terminal_status(&terminal, status, sizeof(status));
	assert(strstr(status, "1-") && strstr(status, "Back"));
	for (index = 0; index < sizeof(phone_keys) / sizeof(phone_keys[0]);
	     ++index, time += 1000)
		hold(&terminal, phone_keys[index], time, 700);
	for (index = 0;
	     index < sizeof(keyboard_keys) / sizeof(keyboard_keys[0]); ++index)
		fplinux_terminal_key(&terminal, keyboard_keys[index], 'x',
				     TSM_CONTROL_MASK | TSM_ALT_MASK, 'x');
	fplinux_terminal_tick(&terminal, time, true);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_HELP);
	assert(!terminal.diagnostic_requested);
	assert(!terminal.modifier_panel && !terminal.history);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	assert(terminal.modifier == (TSM_CONTROL_MASK | TSM_ALT_MASK));
	output_is(&terminal, "", 0);
	press(&terminal, KEY_F14, time + 10);
	focused_menu_is(&terminal, "Help");
	press(&terminal, KEY_UP, time + 11);
	focused_menu_is(&terminal, "Search Ctrl+R");
	press(&terminal, KEY_DOWN, time + 12);
	focused_menu_is(&terminal, "Help");
	press(&terminal, KEY_F13, time + 20);
	fplinux_terminal_key(&terminal, XKB_KEY_Escape, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	focused_menu_is(&terminal, "Help");
	press(&terminal, KEY_F14, time + 30);
	press(&terminal, KEY_NUMERIC_2, time + 40);
	press(&terminal, KEY_NUMERIC_2, time + 50);
	press(&terminal, KEY_OK, time + 60);
	output_is(&terminal, "\033\003\r", 3);
	arm_modifiers(&terminal, "1", time + 70);
	press(&terminal, KEY_NUMERIC_2, time + 610);
	press(&terminal, KEY_F13, time + 620);
	choose_menu(&terminal, "Help", time + 630);
	fplinux_terminal_tick(&terminal, time + 2000, true);
	fplinux_terminal_key(&terminal, XKB_KEY_Escape, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	fplinux_terminal_key(&terminal, XKB_KEY_Escape, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	assert(fplinux_terminal_timeout(&terminal, time + 3000) == 0);
	fplinux_terminal_tick(&terminal, time + 3000, true);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	assert(terminal.modifier == TSM_CONTROL_MASK);
	output_is(&terminal, "", 0);
	/* A queued phone key can predate resuming from the keyboard's Escape. */
	press(&terminal, KEY_NUMERIC_2, time + 2990);
	assert(fplinux_terminal_preedit(&terminal) == 'b');
	assert(terminal.modifier == TSM_CONTROL_MASK);
	output_is(&terminal, "", 0);
	press(&terminal, KEY_F13, time + 3010);
	choose_menu(&terminal, "Help", time + 3020);
	fplinux_terminal_focus(&terminal, false, time + 3030);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED);
	assert(!terminal.modifier && !fplinux_terminal_preedit(&terminal));
	fplinux_terminal_destroy(&terminal);
}

static void verify_case_and_numeric_holds(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	press(&terminal, KEY_NUMERIC_2, 10);
	press(&terminal, KEY_NUMERIC_STAR, 20);
	assert(terminal.mode == FPLINUX_TERMINAL_UPPER);
	assert(fplinux_terminal_preedit(&terminal) == 'A');
	output_is(&terminal, "", 0);
	press(&terminal, KEY_NUMERIC_2, 30);
	assert(fplinux_terminal_preedit(&terminal) == 'B');
	press(&terminal, KEY_NUMERIC_STAR, 40);
	assert(fplinux_terminal_preedit(&terminal) == 'b');
	press(&terminal, KEY_NUMERIC_STAR, 50);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, true, false, 100,
			       true);
	assert(fplinux_terminal_timeout(&terminal, 749) == 1);
	fplinux_terminal_tick(&terminal, 749, true);
	assert(terminal.mode == FPLINUX_TERMINAL_UPPER);
	output_is(&terminal, "", 0);
	fplinux_terminal_tick(&terminal, 750, true);
	assert(terminal.mode == FPLINUX_TERMINAL_NUMERIC);
	assert(fplinux_terminal_preedit(&terminal) == 'B');
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, true, true, 800,
			       true);
	fplinux_terminal_tick(&terminal, 1600, true);
	output_is(&terminal, "", 0);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, false, false, 1700,
			       true);
	press(&terminal, KEY_NUMERIC_2, 1710);
	output_is(&terminal, "B2", 2);
	hold(&terminal, KEY_NUMERIC_STAR, 1800, 650);
	assert(terminal.mode == FPLINUX_TERMINAL_UPPER);
	hold(&terminal, KEY_NUMERIC_STAR, 2500, 650);
	assert(terminal.mode == FPLINUX_TERMINAL_NUMERIC);
	press(&terminal, KEY_NUMERIC_STAR, 3200);
	assert(terminal.mode == FPLINUX_TERMINAL_LOWER);
	press(&terminal, KEY_NUMERIC_2, 3210);
	fplinux_terminal_tick(&terminal, 3910, true);
	output_is(&terminal, "a", 1);
	fplinux_terminal_destroy(&terminal);
}

static void verify_modifier_panel_and_one_shot_input(void)
{
	struct fplinux_terminal terminal;
	char status[64];

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	press(&terminal, KEY_NUMERIC_2, 10);
	fplinux_terminal_phone(&terminal, KEY_F13, true, false, 100, true);
	assert(fplinux_terminal_timeout(&terminal, 599) == 1);
	fplinux_terminal_tick(&terminal, 599, true);
	assert(!terminal.modifier_panel);
	fplinux_terminal_tick(&terminal, 600, true);
	assert(terminal.modifier_panel);
	fplinux_terminal_phone(&terminal, KEY_F13, false, false, 610, true);
	assert(terminal.modifier_panel);
	fplinux_terminal_status(&terminal, status, sizeof(status));
	assert(!memcmp(status, "Done", 4) && strstr(status, "Cancel"));
	press(&terminal, KEY_OK, 620);
	press(&terminal, KEY_RIGHT, 630);
	press(&terminal, KEY_OK, 640);
	press(&terminal, KEY_LEFT, 650);
	assert(terminal.modifier_index == 0);
	assert(terminal.modifier_selection ==
	       (TSM_CONTROL_MASK | TSM_ALT_MASK));
	fplinux_terminal_tick(&terminal, 2000, true);
	output_is(&terminal, "", 0);
	press(&terminal, KEY_F13, 2010);
	assert(terminal.modifier == (TSM_CONTROL_MASK | TSM_ALT_MASK));
	press(&terminal, KEY_NUMERIC_2, 2020);
	press(&terminal, KEY_NUMERIC_2, 2030);
	assert(fplinux_terminal_preedit(&terminal) == 'c');
	output_is(&terminal, "", 0);
	fplinux_terminal_tick(&terminal, 2730, true);
	output_is(&terminal, "\033\003", 2);
	assert(terminal.modifier == 0);
	press(&terminal, KEY_NUMERIC_2, 2740);
	press(&terminal, KEY_OK, 2750);
	output_is(&terminal, "a\r", 2);

	arm_modifiers(&terminal, "123", 2800);
	press(&terminal, KEY_LEFT, 3400);
	output_is(&terminal, "\033[1;8D", 6);
	press(&terminal, KEY_LEFT, 3410);
	output_is(&terminal, "\033[D", 3);

	arm_modifiers(&terminal, "12", 3500);
	press(&terminal, KEY_NUMERIC_STAR, 4100);
	press(&terminal, KEY_F13, 4110);
	fplinux_terminal_tick(&terminal, 5000, true);
	choose_menu(&terminal, "Special keys...", 5100);
	choose_menu(&terminal, "F1", 5110);
	output_is(&terminal, "\033[1;7P", 6);
	assert(!terminal.diagnostic_requested && !terminal.modifier);

	arm_modifiers(&terminal, "2", 5200);
	fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x');
	output_is(&terminal, "x", 1);
	assert(terminal.modifier == TSM_ALT_MASK);
	fplinux_terminal_key(&terminal, XKB_KEY_c, 'c', TSM_CONTROL_MASK, 'c');
	output_is(&terminal, "\003", 1);
	assert(terminal.modifier == TSM_ALT_MASK);
	hold(&terminal, KEY_NUMERIC_STAR, 5800, 650);
	press(&terminal, KEY_NUMERIC_2, 6500);
	output_is(&terminal, "\0332", 2);
	press(&terminal, KEY_NUMERIC_2, 6510);
	output_is(&terminal, "2", 1);

	arm_modifiers(&terminal, "1", 6600);
	fplinux_terminal_status(&terminal, status, sizeof(status));
	assert(strstr(status, "Clear"));
	press(&terminal, KEY_F14, 7200);
	assert(!terminal.modifier && !terminal.history);
	press(&terminal, KEY_F13, 7210);
	focused_menu_is(&terminal, "Modifiers");
	press(&terminal, KEY_F13, 7220);
	assert(terminal.modifier_panel);
	press(&terminal, KEY_NUMERIC_3, 7230);
	press(&terminal, KEY_F14, 7240);
	assert(!terminal.modifier_panel && !terminal.modifier_selection);
	assert(!terminal.modifier && !terminal.history);
	arm_modifiers(&terminal, "2", 7300);
	hold(&terminal, KEY_F13, 7900, 500);
	press(&terminal, KEY_F14, 8410);
	assert(!terminal.modifier);
	output_is(&terminal, "", 0);
	fplinux_terminal_destroy(&terminal);
}

static void verify_xterm_function_keys(void)
{
	static const struct {
		uint32_t keysym;
		unsigned int modifiers;
		const char *bytes;
	} cases[] = {
		{ XKB_KEY_F1, 0, "\033OP" },
		{ XKB_KEY_F1, TSM_SHIFT_MASK, "\033[1;2P" },
		{ XKB_KEY_F1, TSM_CONTROL_MASK, "\033[1;5P" },
		{ XKB_KEY_F1, TSM_ALT_MASK, "\033[1;3P" },
		{ XKB_KEY_F1, TSM_SHIFT_MASK | TSM_CONTROL_MASK, "\033[1;6P" },
		{ XKB_KEY_F13, 0, "\033[1;2P" },
		{ XKB_KEY_F14, 0, "\033[1;2Q" },
		{ XKB_KEY_F24, 0, "\033[24;2~" },
		{ XKB_KEY_F24, TSM_ALT_MASK, "\033[24;4~" },
	};
	struct fplinux_terminal terminal;
	size_t index;

	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		fplinux_terminal_key(&terminal, cases[index].keysym,
				     TSM_VTE_INVALID, cases[index].modifiers,
				     TSM_VTE_INVALID);
		output_is(&terminal, cases[index].bytes,
			  strlen(cases[index].bytes));
	}
	fplinux_terminal_destroy(&terminal);
}

static void verify_history_during_output(void)
{
	struct fplinux_terminal terminal;
	const struct tsm_screen_cell *cells;
	unsigned int line;

	assert(fplinux_terminal_init(&terminal, 4, 2, "test"));
	feed(&terminal, "one\r\ntwo\r\ntri\r\n");
	press(&terminal, KEY_F14, 10);
	press(&terminal, KEY_UP, 20);
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 't' && cells[1].ch == 'w');
	feed(&terminal, "four\r\n");
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[0].ch == 't' && cells[1].ch == 'w');
	assert(fplinux_terminal_history_offset(&terminal) == 2);
	output_is(&terminal, "", 0);
	fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x');
	assert(!terminal.history);
	output_is(&terminal, "x", 1);
	press(&terminal, KEY_F14, 30);
	press(&terminal, KEY_UP, 40);
	press(&terminal, KEY_OK, 50);
	assert(!terminal.history);
	output_is(&terminal, "\r", 1);
	for (line = 0; line < 600; ++line)
		feed(&terminal, "line\r\n");
	assert(tsm_screen_sb_get_line_count(terminal.screen) == 512);
	fplinux_terminal_destroy(&terminal);
}

static uint64_t now_ms(void)
{
	struct timespec time;

	assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
	return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void pump(struct fplinux_terminal *terminal,
		 struct fplinux_terminal_pty *pty, const char *expected)
{
	uint64_t deadline = now_ms() + 3000;
	char captured[16384] = { 0 };
	size_t used = 0;

	while (now_ms() < deadline) {
		struct pollfd descriptor = { .fd = pty->fd, .events = POLLIN };
		ssize_t size;

		if (terminal->output_size) {
			size = write(pty->fd, terminal->output,
				     terminal->output_size);
			if (size > 0)
				fplinux_terminal_consume(terminal,
							 (size_t)size);
			else
				assert(errno == EAGAIN || errno == EINTR);
		}
		assert(poll(&descriptor, 1, 20) >= 0);
		if (!(descriptor.revents & POLLIN))
			continue;
		size = read(pty->fd, captured + used,
			    sizeof(captured) - used - 1);
		assert(size > 0);
		fplinux_terminal_feed(terminal, captured + used, (size_t)size);
		used += (size_t)size;
		captured[used] = '\0';
		if (strstr(captured, expected))
			return;
		assert(used < sizeof(captured) - 1);
	}
	fprintf(stderr, "missing PTY output %s; received: %s\n", expected,
		captured);
	abort();
}

static void type_text(struct fplinux_terminal *terminal, const char *text)
{
	for (; *text; ++text)
		fplinux_terminal_key(terminal, (unsigned char)*text,
				     (unsigned char)*text, 0,
				     (unsigned char)*text);
}

static void verify_bash_line_editing(const char *startup)
{
	struct fplinux_terminal terminal;
	struct fplinux_terminal_pty pty;
	unsigned int position;
	FILE *completion;
	char home[] = "/tmp/fplinux-terminal-test-XXXXXX";
	char history[128];
	int previous_directory = open(".", O_RDONLY | O_CLOEXEC);

	assert(previous_directory >= 0 && mkdtemp(home));
	assert(setenv("HOME", home, 1) == 0);
	snprintf(history, sizeof(history), "%s/.bash_history", home);
	assert(setenv("HISTFILE", history, 1) == 0);
	assert(chdir(home) == 0);
	completion = fopen("completion-target", "wx");
	assert(completion);
	assert(fclose(completion) == 0);
	assert(fplinux_terminal_init(&terminal, 21, 12, "test"));
	assert(fplinux_terminal_pty_open(&pty, 21, 12, "/bin/bash", startup,
					 "test"));
	pump(&terminal, &pty, "\033]777;test;B\a");
	assert(terminal.editing && fplinux_terminal_pty_shell_foreground(&pty));
	type_text(
		&terminal,
		"bind -x '\"\\e[98~\":printf \"<LINE:%s>\\n\" \"$READLINE_LINE\"'");
	press(&terminal, KEY_OK, 1);
	pump(&terminal, &pty, "\033]777;test;B\a");
	for (position = 0; position < 3; ++position) {
		type_text(&terminal, "printf 'BAD-LONG-WRAPPED-LINE\\n'");
		if (position == 0)
			fplinux_terminal_key(&terminal, XKB_KEY_a, 'a',
					     TSM_CONTROL_MASK, 'a');
		else if (position == 1)
			fplinux_terminal_key(&terminal, XKB_KEY_Left,
					     TSM_VTE_INVALID, 0,
					     TSM_VTE_INVALID);
		fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true,
				       false, 100, true);
		fplinux_terminal_tick(&terminal, 750, true);
		fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false,
				       false, 800, true);
		type_text(&terminal, "\033[98~");
		pump(&terminal, &pty, "<LINE:>\r\n");
		type_text(&terminal, "printf 'GOOD\\n'");
		press(&terminal, KEY_OK, 900);
		pump(&terminal, &pty, "GOOD\r\n");
		if (!terminal.editing)
			pump(&terminal, &pty, "\033]777;test;B\a");
		assert(terminal.editing);
	}
	type_text(&terminal, "printf '%s\\n' ./completion");
	press(&terminal, KEY_PICKUP_PHONE, 910);
	press(&terminal, KEY_OK, 920);
	pump(&terminal, &pty, "./completion-target\r\n");
	if (!terminal.editing)
		pump(&terminal, &pty, "\033]777;test;B\a");
	type_text(&terminal, "printf 'HISTORY-ONE\\n'");
	press(&terminal, KEY_OK, 930);
	pump(&terminal, &pty, "HISTORY-ONE\r\n");
	if (!terminal.editing)
		pump(&terminal, &pty, "\033]777;test;B\a");
	press(&terminal, KEY_UP, 940);
	press(&terminal, KEY_OK, 950);
	pump(&terminal, &pty, "HISTORY-ONE\r\n");
	if (!terminal.editing)
		pump(&terminal, &pty, "\033]777;test;B\a");
	fplinux_terminal_key(&terminal, XKB_KEY_r, 'r', TSM_CONTROL_MASK, 'r');
	type_text(&terminal, "HISTORY");
	press(&terminal, KEY_OK, 960);
	pump(&terminal, &pty, "HISTORY-ONE\r\n");
	if (!terminal.editing)
		pump(&terminal, &pty, "\033]777;test;B\a");
	type_text(&terminal,
		  "printf '\\e]133;B\\a\\e]777;unrelated;B\\a'; "
		  "read -rs -n 1 value; printf 'GOT:%s\\n' \"$value\"");
	press(&terminal, KEY_OK, 1000);
	pump(&terminal, &pty, "\033]777;unrelated;B\a");
	assert(!terminal.editing &&
	       fplinux_terminal_pty_shell_foreground(&pty));
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true, false, 1100,
			       true);
	fplinux_terminal_tick(&terminal, 1750, true);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false, false, 1800,
			       true);
	output_is(&terminal, "", 0);
	type_text(&terminal, "q");
	pump(&terminal, &pty, "GOT:q\r\n");
	fplinux_terminal_pty_close(&pty);
	fplinux_terminal_destroy(&terminal);
	assert(unlink("completion-target") == 0);
	assert(unlink(history) == 0 || errno == ENOENT);
	assert(fchdir(previous_directory) == 0);
	close(previous_directory);
	assert(rmdir(home) == 0);
}

static void verify_render_geometry(const char *font_path, unsigned int width,
				   unsigned int height)
{
	struct fplinux_terminal_font font;
	struct fplinux_terminal terminal;
	unsigned int stride = width + 4;
	size_t count = (size_t)stride * (height + 2);
	uint16_t *pixels = malloc(count * sizeof(*pixels));
	struct fplinux_terminal_surface surface = {
		.pixels = pixels + stride,
		.width = width,
		.height = height,
		.stride_bytes = stride * sizeof(*pixels),
	};
	size_t index;
	unsigned int row;
	unsigned int column;
	unsigned int rows;
	unsigned int input_y;

	assert(pixels);
	for (index = 0; index < count; ++index)
		pixels[index] = 0xa55a;
	assert(fplinux_terminal_font_open(&font, font_path));
	assert(fplinux_terminal_init(&terminal, width / font.width,
				     height / font.height - 1, "test"));
	feed(&terminal, "\033[31mA\033[0m");
	press(&terminal, KEY_NUMERIC_2, 1);
	fplinux_terminal_render(&terminal, &font, &surface);
	/* Each fixture glyph starts with a lit left pixel. */
	assert(surface.pixels[0] == 0xc800);
	assert(surface.pixels[1] == 0x1082);
	assert(surface.pixels[font.width] == 0xdedb);
	assert(surface.pixels[font.width + 1] == 0x3186);
	press(&terminal, KEY_NUMERIC_POUND, 10);
	feed(&terminal, "\033[2J\033[999;1H\033[31mA\033[0m");
	press(&terminal, KEY_NUMERIC_2, 20);
	rows = tsm_screen_get_height(terminal.screen);
	hold(&terminal, KEY_F13, 30, 500);
	fplinux_terminal_render(&terminal, &font, &surface);
	input_y = (rows - 2) * font.height;
	assert(tsm_screen_get_height(terminal.screen) == rows);
	assert(tsm_screen_get_cursor_y(terminal.screen) == rows - 1);
	assert(surface.pixels[input_y * stride] == 0xc800);
	assert(surface.pixels[input_y * stride + font.width] == 0xdedb);
	assert(surface.pixels[(height - 2 * font.height) * stride] == 0x528a);
	press(&terminal, KEY_NUMERIC_1, 540);
	press(&terminal, KEY_F13, 550);
	fplinux_terminal_render(&terminal, &font, &surface);
	assert(surface.pixels[input_y * stride] == 0xc800);
	assert(surface.pixels[input_y * stride + font.width] == 0xdedb);
	press(&terminal, KEY_F14, 560);
	fplinux_terminal_render(&terminal, &font, &surface);
	input_y = (rows - 1) * font.height;
	assert(surface.pixels[input_y * stride] == 0xc800);
	assert(surface.pixels[input_y * stride + font.width] == 0xdedb);
	assert(tsm_screen_get_height(terminal.screen) == rows);
	for (column = 0; column < stride; ++column) {
		assert(pixels[column] == 0xa55a);
		assert(pixels[(height + 1) * stride + column] == 0xa55a);
	}
	for (row = 0; row < height; ++row)
		for (column = width; column < stride; ++column)
			assert(surface.pixels[row * stride + column] == 0xa55a);
	fplinux_terminal_focus(&terminal, false, 570);
	feed(&terminal, "\033[Hother");
	fplinux_terminal_render(&terminal, &font, &surface);
	assert(surface.pixels[input_y * stride] == 0xc800);
	fplinux_terminal_destroy(&terminal);
	fplinux_terminal_font_close(&font);
	free(pixels);
}

static unsigned char
rendered_character(const struct fplinux_terminal_surface *surface,
		   const struct fplinux_terminal_font *font, unsigned int x,
		   unsigned int y)
{
	unsigned int stride = surface->stride_bytes / sizeof(*surface->pixels);
	unsigned int character = 0;
	unsigned int bit;

	assert(y + font->height <= surface->height);
	assert(x + font->width <= surface->width);
	for (bit = 0; bit < 7; ++bit) {
		size_t pixel = (size_t)(y + bit + 1) * stride + x;

		if (surface->pixels[pixel] != surface->pixels[pixel + 1])
			character |= 1U << bit;
	}
	return character ? character : ' ';
}

static void rendered_text_is(const struct fplinux_terminal_surface *surface,
			     const struct fplinux_terminal_font *font,
			     unsigned int x, unsigned int y, const char *text)
{
	for (; *text; ++text, x += font->width) {
		unsigned char character =
			rendered_character(surface, font, x, y);

		if (character != (unsigned char)*text)
			fprintf(stderr,
				"pixel text at %u,%u: expected %c, got %u\n", x,
				y, *text, character);
		assert(character == (unsigned char)*text);
	}
}

static size_t rendered_line(const struct fplinux_terminal_surface *surface,
			    const struct fplinux_terminal_font *font,
			    unsigned int row, char *text, size_t size)
{
	size_t columns = surface->width / font->width;
	size_t column;

	assert(columns < size);
	for (column = 0; column < columns; ++column)
		text[column] = rendered_character(surface, font,
						  column * font->width,
						  row * font->height);
	while (columns && text[columns - 1] == ' ')
		--columns;
	text[columns] = '\0';
	return columns;
}

static void verify_menu_rendering(const char *font_path, unsigned int width,
				  unsigned int height)
{
	struct fplinux_terminal_font font;
	struct fplinux_terminal terminal;
	unsigned int stride = width + 4;
	size_t count = (size_t)stride * (height + 2);
	uint16_t *pixels = malloc(count * sizeof(*pixels));
	struct fplinux_terminal_surface surface = {
		.pixels = pixels + stride,
		.width = width,
		.height = height,
		.stride_bytes = stride * sizeof(*pixels),
	};
	unsigned int row;
	unsigned int column;
	size_t index;

	assert(pixels);
	for (index = 0; index < count; ++index)
		pixels[index] = 0xa55a;
	assert(fplinux_terminal_font_open(&font, font_path));
	assert(fplinux_terminal_init(&terminal, width / font.width,
				     height / font.height - 1, "test"));
	arm_modifiers(&terminal, "12", 10);
	press(&terminal, KEY_F13, 550);
	fplinux_terminal_render(&terminal, &font, &surface);
	rendered_text_is(&surface, &font, 0, 0, "Modifiers");
	assert(surface.pixels[1] == 0x3186);
	rendered_text_is(&surface, &font, 0, font.height, "Symbols...");
	press(&terminal, KEY_UP, 560);
	fplinux_terminal_render(&terminal, &font, &surface);
	row = 8;
	rendered_text_is(&surface, &font, 0, row * font.height,
			 "Diagnostic console");
	assert(surface.pixels[row * font.height * stride + 1] == 0x3186);
	rendered_text_is(&surface, &font, 0, 0, "Modifiers");
	choose_menu(&terminal, "Special keys...", 570);
	press(&terminal, KEY_UP, 580);
	focused_menu_is(&terminal, "F12");
	fplinux_terminal_render(&terminal, &font, &surface);
	rendered_text_is(&surface, &font, 0, 0, "Special keys");
	row = width == 128 ? 10 : 13;
	rendered_text_is(&surface, &font, 0, row * font.height, "F12");
	for (column = 0; column < stride; ++column) {
		assert(pixels[column] == 0xa55a);
		assert(pixels[(height + 1) * stride + column] == 0xa55a);
	}
	for (row = 0; row < height; ++row)
		for (column = width; column < stride; ++column)
			assert(surface.pixels[row * stride + column] == 0xa55a);
	fplinux_terminal_destroy(&terminal);
	fplinux_terminal_font_close(&font);
	free(pixels);
}

struct help_render_metrics {
	unsigned int scroll_steps;
	unsigned int first_view_lines;
	size_t first_line_size;
};

struct help_view {
	char lines[80][80];
	unsigned int first_line;
	unsigned int last_line;
	unsigned int total_lines;
};

static void read_help_view(struct fplinux_terminal *terminal,
			   const struct fplinux_terminal_surface *surface,
			   const struct fplinux_terminal_font *font,
			   unsigned int section, unsigned int body_rows,
			   struct help_view *view)
{
	static const char *const titles[] = { "Typing", "Modifiers",
					      "History" };
	unsigned int columns = surface->width / font->width;
	unsigned int displayed_section;
	unsigned int displayed_total;
	unsigned int row;
	unsigned int column;
	char text[80];
	char title[32];
	char *counter;
	int consumed = 0;

	memset(view, 0, sizeof(*view));
	assert(body_rows < sizeof(view->lines) / sizeof(view->lines[0]));
	fplinux_terminal_render(terminal, font, surface);
	rendered_line(surface, font, 0, text, sizeof(text));
	assert(sscanf(text, "%31s %u/%u", title, &displayed_section,
		      &displayed_total) == 3);
	assert(!strcmp(title, titles[section]));
	assert(displayed_section == section + 1 && displayed_total == 3);
	for (row = 0; row < body_rows; ++row)
		rendered_line(surface, font, row + 1, view->lines[row],
			      sizeof(view->lines[row]));
	assert(columns < sizeof(text));
	for (column = 0; column < columns; ++column)
		text[column] =
			rendered_character(surface, font, column * font->width,
					   surface->height - font->height);
	text[columns] = '\0';
	assert(!strcmp(text + columns - 4, "Back"));
	text[columns - 4] = '\0';
	counter = text;
	while (*counter == ' ')
		++counter;
	if (*counter) {
		assert(sscanf(counter, "%u-%u/%u%n", &view->first_line,
			      &view->last_line, &view->total_lines,
			      &consumed) == 3);
		for (counter += consumed; *counter; ++counter)
			assert(*counter == ' ');
	}
}

static void help_text_matches(const char *text, const char **cursor)
{
	for (; *text; ++text) {
		if (*text == ' ')
			continue;
		while (**cursor == ' ')
			++*cursor;
		if (*text != **cursor)
			fprintf(stderr, "help text: expected %.32s, got %s\n",
				*cursor, text);
		assert(*text == **cursor);
		++*cursor;
	}
}

static struct help_render_metrics verify_help_rendering(const char *font_path,
							unsigned int width,
							unsigned int height,
							bool modifiers,
							const char *first_line)
{
	/* Complete instructions must survive wrapping and vertical scrolling. */
	static const char *const expected[] = {
		"Use Left/Right to switch sections and Up/Down to scroll the text. "
		"2-9: Tap repeatedly to choose a letter. Pause or press another digit to send it. "
		"1: Choose punctuation. 0: Insert a space. "
		"*: Switch letter case. Hold * to switch between numbers and letters. "
		"#: Cancel a pending letter, otherwise erase. Hold # to clear the line at the Bash prompt. "
		"Centre: Enter. Dial: Tab completion. "
		"Menu: Choose symbols, special keys or the input language.",
		"Hold left soft or choose Menu > Modifiers to open the strip. "
		"Left/Right: Select Ctrl, Alt or Shift. Centre: Toggle it. "
		"1: Toggle Ctrl. 2: Toggle Alt. 3: Toggle Shift. You can combine them. "
		"Left soft (Done): Arm the combination for the next phone key or completed letter. "
		"Right soft (Cancel or Clear): Reset the combination outside menus. "
		"Back in a menu keeps modifiers armed. Pending letters wait while menus are open.",
		"Right soft: Open scrollback when no modifiers are armed. "
		"Up/Down: Scroll one line. Left/Right: Scroll one page. "
		"Right soft (Back): Return to live output. Typing also resumes live input. "
		"External keyboard: Shift+PageUp and Shift+PageDown scroll output. "
		"At the Bash prompt, Up/Down choose previous or next commands. "
		"Menu > Search sends Ctrl+R to search command history.",
	};
	struct help_render_metrics metrics = { 0 };
	struct fplinux_terminal_font font;
	struct fplinux_terminal terminal;
	unsigned int stride = width + 4;
	size_t count = (size_t)stride * (height + 2);
	uint16_t *pixels = malloc(count * sizeof(*pixels));
	struct fplinux_terminal_surface surface = {
		.pixels = pixels + stride,
		.width = width,
		.height = height,
		.stride_bytes = stride * sizeof(*pixels),
	};
	struct help_view view;
	struct help_view previous;
	char lines[128][80];
	char history_first_line[80];
	unsigned int body_rows;
	unsigned int section;
	unsigned int row;
	unsigned int column;
	uint64_t time = 600;
	size_t index;

	assert(pixels);
	for (index = 0; index < count; ++index)
		pixels[index] = 0xa55a;
	assert(fplinux_terminal_font_open(&font, font_path));
	assert(fplinux_terminal_init(&terminal, width / font.width,
				     height / font.height - 1, "test"));
	if (modifiers)
		arm_modifiers(&terminal, "12", 10);
	press(&terminal, KEY_F13, 550);
	choose_menu(&terminal, "Help", 560);
	body_rows = height / font.height - (modifiers ? 3 : 2);
	for (section = 0; section < 3; ++section) {
		const char *cursor = expected[section];
		unsigned int offset = 0;
		unsigned int line_count = 0;
		unsigned int displayed_lines;

		read_help_view(&terminal, &surface, &font, section, body_rows,
			       &view);
		displayed_lines = view.total_lines;
		assert(view.first_line == (displayed_lines ? 1 : 0));
		assert(view.last_line == (displayed_lines ? body_rows : 0));
		if (!section) {
			metrics.first_line_size = strlen(view.lines[0]);
			assert(!strcmp(view.lines[0], first_line));
		}
		if (section == 2)
			strcpy(history_first_line, view.lines[0]);
		for (row = 0; row < body_rows; ++row) {
			strcpy(lines[row], view.lines[row]);
			help_text_matches(view.lines[row], &cursor);
			if (view.lines[row][0])
				++line_count;
		}
		if (!section)
			metrics.first_view_lines = line_count;
		for (;;) {
			previous = view;
			if (offset % 2)
				fplinux_terminal_key(&terminal, XKB_KEY_Down,
						     TSM_VTE_INVALID, 0,
						     TSM_VTE_INVALID);
			else
				press(&terminal, KEY_DOWN, time++);
			read_help_view(&terminal, &surface, &font, section,
				       body_rows, &view);
			if (!memcmp(&previous, &view, sizeof(view)))
				break;
			assert(displayed_lines);
			++offset;
			assert(offset + body_rows < 128);
			assert(view.first_line == offset + 1);
			assert(view.last_line == offset + body_rows);
			assert(view.total_lines == displayed_lines);
			for (row = 0; row + 1 < body_rows; ++row)
				assert(!strcmp(view.lines[row],
					       lines[offset + row]));
			strcpy(lines[offset + body_rows - 1],
			       view.lines[body_rows - 1]);
			assert(view.lines[body_rows - 1][0]);
			help_text_matches(view.lines[body_rows - 1], &cursor);
			++line_count;
		}
		assert(!*cursor);
		assert(displayed_lines == (offset ? line_count : 0));
		metrics.scroll_steps += offset;
		while (offset) {
			if (offset % 2)
				fplinux_terminal_key(&terminal, XKB_KEY_Up,
						     TSM_VTE_INVALID, 0,
						     TSM_VTE_INVALID);
			else
				press(&terminal, KEY_UP, time++);
			--offset;
			read_help_view(&terminal, &surface, &font, section,
				       body_rows, &view);
			assert(view.first_line == offset + 1);
			assert(view.last_line == offset + body_rows);
			assert(view.total_lines == line_count);
			for (row = 0; row < body_rows; ++row)
				assert(!strcmp(view.lines[row],
					       lines[offset + row]));
		}
		previous = view;
		press(&terminal, KEY_UP, time++);
		read_help_view(&terminal, &surface, &font, section, body_rows,
			       &view);
		assert(!memcmp(&previous, &view, sizeof(view)));
		press(&terminal, KEY_DOWN, time++);
		if (section % 2)
			fplinux_terminal_key(&terminal, XKB_KEY_Right,
					     TSM_VTE_INVALID, 0,
					     TSM_VTE_INVALID);
		else
			press(&terminal, KEY_RIGHT, time++);
	}
	read_help_view(&terminal, &surface, &font, 0, body_rows, &view);
	assert(!strcmp(view.lines[0], first_line));
	assert(view.first_line == (view.total_lines ? 1 : 0));
	press(&terminal, KEY_DOWN, time++);
	fplinux_terminal_key(&terminal, XKB_KEY_Left, TSM_VTE_INVALID, 0,
			     TSM_VTE_INVALID);
	read_help_view(&terminal, &surface, &font, 2, body_rows, &view);
	assert(!strcmp(view.lines[0], history_first_line));
	assert(view.first_line == (view.total_lines ? 1 : 0));
	press(&terminal, KEY_DOWN, time++);
	press(&terminal, KEY_LEFT, time++);
	read_help_view(&terminal, &surface, &font, 1, body_rows, &view);
	assert(view.first_line == (view.total_lines ? 1 : 0));
	assert(tsm_screen_get_width(terminal.screen) == width / font.width);
	assert(tsm_screen_get_height(terminal.screen) ==
	       height / font.height - 1);
	output_is(&terminal, "", 0);
	assert(terminal.modifier ==
	       (modifiers ? TSM_CONTROL_MASK | TSM_ALT_MASK : 0));
	press(&terminal, KEY_F14, time++);
	focused_menu_is(&terminal, "Help");
	for (column = 0; column < stride; ++column) {
		assert(pixels[column] == 0xa55a);
		assert(pixels[(height + 1) * stride + column] == 0xa55a);
	}
	for (row = 0; row < height; ++row)
		for (column = width; column < stride; ++column)
			assert(surface.pixels[row * stride + column] == 0xa55a);
	fplinux_terminal_destroy(&terminal);
	fplinux_terminal_font_close(&font);
	free(pixels);
	return metrics;
}

static void verify_adaptive_help(const char *small_font, const char *large_font,
				 const char *tall_font)
{
	struct help_render_metrics small = verify_help_rendering(
		small_font, 128, 160, false, "Use Left/Right to");
	struct help_render_metrics large = verify_help_rendering(
		large_font, 240, 320, false, "Use Left/Right to switch");
	struct help_render_metrics wider = verify_help_rendering(
		small_font, 180, 160, false, "Use Left/Right to switch");
	struct help_render_metrics taller = verify_help_rendering(
		small_font, 128, 244, false, "Use Left/Right to");
	struct help_render_metrics minimum = verify_help_rendering(
		tall_font, 128, 128, false, "Use Left/Right");
	struct help_render_metrics armed = verify_help_rendering(
		tall_font, 128, 128, true, "Use Left/Right");
	struct help_render_metrics fitting = verify_help_rendering(
		small_font, 128, 640, false, "Use Left/Right to");

	assert(large.scroll_steps < small.scroll_steps);
	assert(wider.scroll_steps < small.scroll_steps);
	assert(wider.first_line_size > small.first_line_size);
	assert(taller.scroll_steps < small.scroll_steps);
	assert(taller.first_view_lines > small.first_view_lines);
	assert(armed.scroll_steps > minimum.scroll_steps);
	assert(armed.first_view_lines == 1 && minimum.first_view_lines == 2);
	assert(!fitting.scroll_steps);
	verify_help_rendering(small_font, 128, 160, true, "Use Left/Right to");
	verify_help_rendering(large_font, 240, 320, true,
			      "Use Left/Right to switch");
}

int main(int argc, char **argv)
{
	assert(argc == 5);
	verify_grid_and_colors();
	verify_composition_and_key_sequences();
	verify_pound_and_focus();
	verify_menu_softkey_selection();
	verify_menu_choosers_and_back();
	verify_help_preserves_input();
	verify_case_and_numeric_holds();
	verify_modifier_panel_and_one_shot_input();
	verify_xterm_function_keys();
	verify_history_during_output();
	verify_bash_line_editing(argv[1]);
	verify_render_geometry(argv[2], 128, 160);
	verify_render_geometry(argv[3], 240, 320);
	verify_menu_rendering(argv[2], 128, 160);
	verify_menu_rendering(argv[3], 240, 320);
	verify_adaptive_help(argv[2], argv[3], argv[4]);
	return 0;
}
