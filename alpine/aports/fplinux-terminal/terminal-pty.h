/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_PTY_H
#define FPLINUX_TERMINAL_PTY_H

#include <stdbool.h>
#include <sys/types.h>

struct fplinux_terminal_pty {
	int fd;
	pid_t shell;
};

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup,
			       const char *shell_token);
bool fplinux_terminal_pty_shell_foreground(
	const struct fplinux_terminal_pty *pty);
bool fplinux_terminal_pty_finished(struct fplinux_terminal_pty *pty,
				   int *status);
void fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty);

#endif
