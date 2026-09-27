/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_KEYBOARD_H
#define FPLINUX_TERMINAL_KEYBOARD_H

#include "fplinux-input-session.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

struct fplinux_terminal_keyboard_device {
	uint64_t id;
	bool pressed[KEY_CNT];
};

struct fplinux_terminal_keyboard {
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	unsigned char references[KEY_CNT];
	struct fplinux_terminal_keyboard_device
		devices[FPLINUX_INPUT_SESSION_DEVICE_COUNT];
};

struct fplinux_terminal_key {
	uint32_t keysym;
	uint32_t ascii;
	uint32_t unicode;
	unsigned int modifiers;
	bool repeats;
};

bool fplinux_terminal_keyboard_open(struct fplinux_terminal_keyboard *keyboard,
				    const char *data_root, char *error,
				    size_t error_size);
void fplinux_terminal_keyboard_close(struct fplinux_terminal_keyboard *keyboard);
bool fplinux_terminal_keyboard_event(struct fplinux_terminal_keyboard *keyboard,
				     const struct fplinux_input_event *event,
				     struct fplinux_terminal_key *key);
bool fplinux_terminal_keyboard_repeat(struct fplinux_terminal_keyboard *keyboard,
				      uint64_t device, unsigned int code,
				      struct fplinux_terminal_key *key);
bool fplinux_terminal_keyboard_reset(struct fplinux_terminal_keyboard *keyboard);

#endif
