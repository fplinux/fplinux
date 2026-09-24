/* SPDX-License-Identifier: MIT */
/* Keyboard layout selection and text filtering for MicroPythonOS. */

#ifndef FPLINUX_KEYBOARD_TEXT_H
#define FPLINUX_KEYBOARD_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#define FPLINUX_KEYBOARD_TEXT_DATA_ROOT "/usr/share/fplinux/xkb"
/* XKB has four layout groups and US always occupies the first one. */
#define FPLINUX_KEYBOARD_TEXT_LAYOUT_COUNT 3U
#define FPLINUX_KEYBOARD_TEXT_LAYOUT_NAME_BYTES 32U
#define FPLINUX_KEYBOARD_TEXT_KEYMAP_BYTES 512U

struct fplinux_keyboard_text_layouts {
	char names[FPLINUX_KEYBOARD_TEXT_LAYOUT_COUNT]
		  [FPLINUX_KEYBOARD_TEXT_LAYOUT_NAME_BYTES];
	size_t count;
};

/*
 * Reads the optional layouts registered as files in DATA_ROOT/layouts and
 * sorts them by name. A missing directory registers none and names starting
 * with '.' are ignored. Any other name that is not made of lowercase letters,
 * digits and '_', or more than three layouts, is an error.
 */
bool fplinux_keyboard_text_find_layouts(
	const char *data_root, struct fplinux_keyboard_text_layouts *layouts,
	char *error, size_t error_size);

/*
 * Composes the XKB symbols "pc+us", then "+<layout>:<group>" for each
 * optional layout from group 2, then the Alt+Shift group toggle.
 */
bool fplinux_keyboard_text_symbols(
	const struct fplinux_keyboard_text_layouts *layouts, char *symbols,
	size_t symbols_size);

/* Composes a complete keymap source for xkb_keymap_new_from_string(). */
bool fplinux_keyboard_text_keymap(
	const struct fplinux_keyboard_text_layouts *layouts, char *keymap,
	size_t keymap_size);

/* Accepts non-empty UTF-8 text without C0, DEL or C1 control characters. */
bool fplinux_keyboard_text_is_printable(const char *text);

#endif
