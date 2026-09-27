/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-keyboard.h"

#include <assert.h>
#include <libtsm.h>
#include <stdio.h>

static bool event(struct fplinux_terminal_keyboard *keyboard, uint64_t device,
		  unsigned int code, bool pressed,
		  struct fplinux_terminal_key *key)
{
	struct fplinux_input_event input = {
		.type = FPLINUX_INPUT_EVENT_KEY,
		.source = FPLINUX_INPUT_SOURCE_KEYBOARD,
		.device_id = device,
		.code = code,
		.pressed = pressed,
	};

	return fplinux_terminal_keyboard_event(keyboard, &input, key);
}

int main(int argc, char **argv)
{
	struct fplinux_terminal_keyboard keyboard;
	struct fplinux_terminal_key key;
	struct fplinux_input_event removed = {
		.type = FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
		.source = FPLINUX_INPUT_SOURCE_KEYBOARD,
		.device_id = 1,
	};
	struct fplinux_input_event phone = {
		.type = FPLINUX_INPUT_EVENT_KEY,
		.source = FPLINUX_INPUT_SOURCE_KEYPAD,
		.device_id = 3,
		.code = KEY_F13,
		.pressed = true,
	};
	char error[256];

	assert(argc == 2);
	if (!fplinux_terminal_keyboard_open(&keyboard, argv[1], error,
					    sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	assert(event(&keyboard, 1, KEY_A, true, &key));
	assert(key.unicode == 'a' && key.modifiers == 0 && key.repeats);
	assert(fplinux_terminal_keyboard_repeat(&keyboard, 1, KEY_A, &key));
	assert(key.unicode == 'a');
	assert(!event(&keyboard, 1, KEY_A, false, &key));
	assert(!fplinux_terminal_keyboard_repeat(&keyboard, 1, KEY_A, &key));
	assert(!event(&keyboard, 1, KEY_LEFTSHIFT, true, &key));
	assert(!event(&keyboard, 2, KEY_LEFTSHIFT, true, &key));
	assert(!event(&keyboard, 1, KEY_LEFTSHIFT, false, &key));
	assert(event(&keyboard, 1, KEY_3, true, &key));
	assert(key.unicode == '#' && key.modifiers == TSM_SHIFT_MASK);
	event(&keyboard, 1, KEY_3, false, &key);
	assert(event(&keyboard, 1, KEY_8, true, &key));
	assert(key.unicode == '*');
	event(&keyboard, 1, KEY_8, false, &key);
	removed.device_id = 2;
	assert(!fplinux_terminal_keyboard_event(&keyboard, &removed, &key));
	assert(event(&keyboard, 1, KEY_A, true, &key));
	assert(key.unicode == 'a' && key.modifiers == 0);
	event(&keyboard, 1, KEY_A, false, &key);
	assert(!fplinux_terminal_keyboard_event(&keyboard, &phone, &key));
	assert(event(&keyboard, 1, KEY_F13, true, &key));
	assert(key.keysym == XKB_KEY_F13);
	event(&keyboard, 1, KEY_F13, false, &key);
	event(&keyboard, 1, KEY_LEFTCTRL, true, &key);
	assert(event(&keyboard, 1, KEY_A, true, &key));
	assert(key.ascii == 'a' && key.modifiers == TSM_CONTROL_MASK);
	assert(fplinux_terminal_keyboard_reset(&keyboard));
	assert(!fplinux_terminal_keyboard_repeat(&keyboard, 1, KEY_A, &key));
	assert(event(&keyboard, 4, KEY_A, true, &key));
	assert(key.unicode == 'a' && key.modifiers == 0);
	fplinux_terminal_keyboard_close(&keyboard);
	return 0;
}
