// SPDX-License-Identifier: MIT
/* Keyboard layout selection and text filtering for MicroPythonOS. */

#define _POSIX_C_SOURCE 200809L

#include "fplinux-keyboard-text.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPLINUX_KEYBOARD_TEXT_PATH_BYTES 256U
#define FPLINUX_KEYBOARD_TEXT_FIRST_OPTIONAL_GROUP 2U

static bool is_layout_name(const char *name)
{
	size_t length = strlen(name);
	size_t index;

	if (length == 0 || length >= FPLINUX_KEYBOARD_TEXT_LAYOUT_NAME_BYTES)
		return false;
	for (index = 0; index < length; ++index) {
		char character = name[index];

		if ((character < 'a' || character > 'z') &&
		    (character < '0' || character > '9') && character != '_')
			return false;
	}
	return true;
}

static int compare_layout_names(const void *first, const void *second)
{
	return strcmp(first, second);
}

bool fplinux_keyboard_text_find_layouts(
	const char *data_root, struct fplinux_keyboard_text_layouts *layouts,
	char *error, size_t error_size)
{
	char path[FPLINUX_KEYBOARD_TEXT_PATH_BYTES];
	struct dirent *entry;
	DIR *directory;
	int length;

	memset(layouts, 0, sizeof(*layouts));
	length = snprintf(path, sizeof(path), "%s/layouts", data_root);
	if (length < 0 || (size_t)length >= sizeof(path)) {
		snprintf(error, error_size, "keyboard layout path is too long");
		return false;
	}
	directory = opendir(path);
	if (directory == NULL) {
		if (errno == ENOENT)
			return true;
		snprintf(error, error_size, "cannot open %s: %s", path,
			 strerror(errno));
		return false;
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (entry->d_name[0] == '.')
			continue;
		if (!is_layout_name(entry->d_name)) {
			snprintf(error, error_size,
				 "%s contains an invalid keyboard layout name",
				 path);
			closedir(directory);
			return false;
		}
		if (layouts->count == FPLINUX_KEYBOARD_TEXT_LAYOUT_COUNT) {
			snprintf(
				error, error_size,
				"more than %u optional keyboard layouts are installed",
				FPLINUX_KEYBOARD_TEXT_LAYOUT_COUNT);
			closedir(directory);
			return false;
		}
		memcpy(layouts->names[layouts->count], entry->d_name,
		       strlen(entry->d_name) + 1U);
		++layouts->count;
	}
	if (errno != 0) {
		snprintf(error, error_size, "cannot read %s: %s", path,
			 strerror(errno));
		closedir(directory);
		return false;
	}
	closedir(directory);
	qsort(layouts->names, layouts->count, sizeof(layouts->names[0]),
	      compare_layout_names);
	return true;
}

static bool append_text(char *buffer, size_t size, size_t *used,
			const char *text)
{
	size_t length = strlen(text);

	if (length >= size - *used)
		return false;
	memcpy(buffer + *used, text, length + 1U);
	*used += length;
	return true;
}

bool fplinux_keyboard_text_symbols(
	const struct fplinux_keyboard_text_layouts *layouts, char *symbols,
	size_t symbols_size)
{
	size_t used = 0;
	size_t index;

	if (symbols_size == 0 ||
	    layouts->count > FPLINUX_KEYBOARD_TEXT_LAYOUT_COUNT)
		return false;
	symbols[0] = '\0';
	if (!append_text(symbols, symbols_size, &used, "pc+us"))
		return false;
	for (index = 0; index < layouts->count; ++index) {
		const char group[] = {
			':',
			(char)('0' +
			       FPLINUX_KEYBOARD_TEXT_FIRST_OPTIONAL_GROUP +
			       index),
			'\0',
		};

		if (!is_layout_name(layouts->names[index]) ||
		    !append_text(symbols, symbols_size, &used, "+") ||
		    !append_text(symbols, symbols_size, &used,
				 layouts->names[index]) ||
		    !append_text(symbols, symbols_size, &used, group))
			return false;
	}
	return append_text(symbols, symbols_size, &used,
			   "+group(alt_shift_toggle)");
}

bool fplinux_keyboard_text_keymap(
	const struct fplinux_keyboard_text_layouts *layouts, char *keymap,
	size_t keymap_size)
{
	char symbols[FPLINUX_KEYBOARD_TEXT_KEYMAP_BYTES];
	int length;

	if (!fplinux_keyboard_text_symbols(layouts, symbols, sizeof(symbols)))
		return false;
	length = snprintf(
		keymap, keymap_size,
		"xkb_keymap {\n"
		"\txkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
		"\txkb_types { include \"complete\" };\n"
		"\txkb_compat { include \"complete\" };\n"
		"\txkb_symbols { include \"%s\" };\n"
		"};\n",
		symbols);
	return length >= 0 && (size_t)length < keymap_size;
}

bool fplinux_keyboard_text_is_printable(const char *text)
{
	const unsigned char *byte = (const unsigned char *)text;

	if (*byte == '\0')
		return false;
	for (; *byte != '\0'; ++byte) {
		if (*byte < 0x20U || *byte == 0x7fU)
			return false;
		/* UTF-8 encodes the C1 controls U+0080..U+009F as c2 80..9f. */
		if (*byte == 0xc2U && byte[1] >= 0x80U && byte[1] <= 0x9fU)
			return false;
	}
	return true;
}
