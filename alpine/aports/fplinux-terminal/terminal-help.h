/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_HELP_H
#define FPLINUX_TERMINAL_HELP_H

#include <stddef.h>

/* The help screen's sections, translated: a title and its text. */
size_t fplinux_terminal_help_count(void);
void fplinux_terminal_help_section(size_t index, const char **title,
				   const char **text);

#endif
