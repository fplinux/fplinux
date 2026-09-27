/* SPDX-License-Identifier: GPL-2.0-only */
/* Artifact check: pass an extracted fplinux-xkb APK data root explicitly. */
#include "fplinux-terminal.h"
#include "terminal-keyboard.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool key_event(struct fplinux_terminal_keyboard *keyboard,
		      unsigned int code, bool pressed,
		      struct fplinux_terminal_key *key)
{
	const struct fplinux_input_event event = {
		.type = FPLINUX_INPUT_EVENT_KEY,
		.source = FPLINUX_INPUT_SOURCE_KEYBOARD,
		.device_id = 1,
		.code = code,
		.pressed = pressed,
	};

	return fplinux_terminal_keyboard_event(keyboard, &event, key);
}

int main(int argc, char **argv)
{
	struct fplinux_terminal_keyboard keyboard;
	struct fplinux_terminal terminal;
	struct fplinux_terminal_key key;
	struct fplinux_input_event phone = {
		.type = FPLINUX_INPUT_EVENT_KEY,
		.source = FPLINUX_INPUT_SOURCE_KEYPAD,
		.device_id = 2,
		.code = KEY_F13,
		.pressed = true,
	};
	unsigned int index;
	char error[256];
	const char lines[] = "one\r\ntwo\r\nthree\r\nfour\r\n";
	const char *const sequences[] = {
		"\033[1;2P",  "\033[1;2Q",  "\033[1;2R",  "\033[1;2S",
		"\033[15;2~", "\033[17;2~", "\033[18;2~", "\033[19;2~",
		"\033[20;2~", "\033[21;2~", "\033[23;2~", "\033[24;2~",
	};

	assert(argc == 2);
	if (!fplinux_terminal_keyboard_open(&keyboard, argv[1], error,
					    sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	assert(fplinux_terminal_init(&terminal, 21, 3, "test"));
	for (index = 0; index < 12; ++index) {
		assert(key_event(&keyboard, KEY_F13 + index, true, &key));
		assert(key.keysym == XKB_KEY_F13 + index);
		fplinux_terminal_key(&terminal, key.keysym, key.ascii,
				     key.modifiers, key.unicode);
		assert(terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED);
		assert(!terminal.history);
		if (terminal.output_size != strlen(sequences[index]) ||
		    memcmp(terminal.output, sequences[index],
			   terminal.output_size))
			fprintf(stderr, "F%u: expected %s, received %.*s\n",
				index + 13, sequences[index],
				(int)terminal.output_size, terminal.output);
		assert(terminal.output_size == strlen(sequences[index]));
		assert(!memcmp(terminal.output, sequences[index],
			       terminal.output_size));
		fplinux_terminal_consume(&terminal, terminal.output_size);
		key_event(&keyboard, KEY_F13 + index, false, &key);
	}
	assert(!fplinux_terminal_keyboard_event(&keyboard, &phone, &key));
	fplinux_terminal_phone(&terminal, KEY_F13, true, false, 1, true);
	assert(terminal.menu == FPLINUX_TERMINAL_MENU_MAIN);
	fplinux_terminal_phone(&terminal, KEY_F14, true, false, 2, true);
	fplinux_terminal_feed(&terminal, lines, strlen(lines));
	key_event(&keyboard, KEY_CAPSLOCK, true, &key);
	key_event(&keyboard, KEY_CAPSLOCK, false, &key);
	key_event(&keyboard, KEY_LEFTSHIFT, true, &key);
	assert(key_event(&keyboard, KEY_PAGEUP, true, &key));
	assert((key.modifiers & (TSM_LOCK_MASK | TSM_SHIFT_MASK)) ==
	       (TSM_LOCK_MASK | TSM_SHIFT_MASK));
	fplinux_terminal_key(&terminal, key.keysym, key.ascii, key.modifiers,
			     key.unicode);
	assert(terminal.history &&
	       fplinux_terminal_history_offset(&terminal) > 0);
	assert(terminal.output_size == 0);
	assert(fplinux_terminal_keyboard_reset(&keyboard));
	key_event(&keyboard, KEY_LEFTCTRL, true, &key);
	key_event(&keyboard, KEY_LEFTALT, true, &key);
	assert(key_event(&keyboard, KEY_F1, true, &key));
	assert(key.keysym == XKB_KEY_XF86Switch_VT_1);
	fplinux_terminal_key(&terminal, key.keysym, key.ascii, key.modifiers,
			     key.unicode);
	assert(terminal.diagnostic_requested && terminal.output_size == 0);
	fplinux_terminal_destroy(&terminal);
	fplinux_terminal_keyboard_close(&keyboard);
	return 0;
}
