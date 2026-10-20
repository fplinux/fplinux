/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_PTY_SUPERVISOR_H
#define FPLINUX_TERMINAL_PTY_SUPERVISOR_H

#include <sys/types.h>

#ifndef FPLINUX_TERMINAL_PTY_HELPER_PATH
#define FPLINUX_TERMINAL_PTY_HELPER_PATH "/usr/libexec/fplinux-terminal-pty"
#endif

enum fplinux_terminal_pty_report_type {
	FPLINUX_TERMINAL_PTY_READY,
	FPLINUX_TERMINAL_PTY_FINISHED,
	FPLINUX_TERMINAL_PTY_ERROR,
};

/* Pipe writes are atomic; this private protocol never leaves this package. */
struct fplinux_terminal_pty_report {
	enum fplinux_terminal_pty_report_type type;
	pid_t shell;
	int status;
	int error;
};

#endif
