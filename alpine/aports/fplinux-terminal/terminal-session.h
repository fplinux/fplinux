/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_SESSION_H
#define FPLINUX_TERMINAL_SESSION_H

#include "fplinux-terminal.h"
#include "terminal-pty.h"

/* The engine and PTY outlive this borrowed I/O state. */
struct fplinux_terminal_session {
	struct fplinux_terminal *terminal;
	struct fplinux_terminal_pty *pty;
	bool input_closed;
	bool output_closed;
	bool child_finished;
	bool finished;
	int child_status;
};

void fplinux_terminal_session_init(struct fplinux_terminal_session *session,
				   struct fplinux_terminal *terminal,
				   struct fplinux_terminal_pty *pty);
/* After stream closure, watch the supervisor until its real status arrives. */
int fplinux_terminal_session_fd(const struct fplinux_terminal_session *session);
short fplinux_terminal_session_events(
	const struct fplinux_terminal_session *session);
bool fplinux_terminal_session_can_input(
	const struct fplinux_terminal_session *session);
/* Bounded output draining preserves UI responsiveness and queued input. */
bool fplinux_terminal_session_dispatch(struct fplinux_terminal_session *session,
				       short events);
int fplinux_terminal_session_result(
	const struct fplinux_terminal_session *session);

#endif
