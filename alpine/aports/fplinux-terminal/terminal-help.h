/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_HELP_H
#define FPLINUX_TERMINAL_HELP_H

#include "fplinux-terminal.h"

struct fplinux_terminal_help {
	const char *title;
	const char *cursor;
	unsigned int columns;
	unsigned int body_rows;
	unsigned int remaining_rows;
	unsigned int section;
	unsigned int section_count;
	unsigned int line_count;
	unsigned int offset;
	unsigned int max_offset;
};

void fplinux_terminal_help_open(const struct fplinux_terminal *terminal,
				struct fplinux_terminal_help *help);
size_t fplinux_terminal_help_next(struct fplinux_terminal_help *help,
				  const char **text);

#endif
