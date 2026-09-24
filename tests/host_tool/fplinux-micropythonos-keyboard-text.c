/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host driver for MicroPythonOS keyboard layout selection. It prints what the
 * production functions compose so that the Python test owns the expectations;
 * it does not load xkbcommon, MicroPython or an input device.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fplinux-keyboard-text.h"

static int print_symbols(const char *data_root)
{
	struct fplinux_keyboard_text_layouts layouts;
	char text[FPLINUX_KEYBOARD_TEXT_KEYMAP_BYTES];
	char error[256];

	if (!fplinux_keyboard_text_find_layouts(data_root, &layouts, error,
						sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return EXIT_FAILURE;
	}
	if (!fplinux_keyboard_text_symbols(&layouts, text, sizeof(text))) {
		fprintf(stderr, "composition failed\n");
		return EXIT_FAILURE;
	}
	fputs(text, stdout);
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s symbols|printable ARGUMENT\n",
			argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "symbols") == 0)
		return print_symbols(argv[2]);
	if (strcmp(argv[1], "printable") == 0)
		return fplinux_keyboard_text_is_printable(argv[2]) ?
			       EXIT_SUCCESS :
			       EXIT_FAILURE;
	fprintf(stderr, "unknown mode %s\n", argv[1]);
	return 2;
}
