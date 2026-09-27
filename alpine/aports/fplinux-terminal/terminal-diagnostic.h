/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_DIAGNOSTIC_H
#define FPLINUX_TERMINAL_DIAGNOSTIC_H

#include "fplinux-input-session.h"

struct fplinux_terminal_diagnostic {
	int fds[FPLINUX_INPUT_SESSION_DEVICE_COUNT];
	bool right_pressed[FPLINUX_INPUT_SESSION_DEVICE_COUNT];
	size_t count;
};

/*
 * Begin before suspending input; dispatch only while the session is suspended.
 * The retained evdev descriptors inherit libinput's nonblocking mode.
 */
bool fplinux_terminal_diagnostic_begin(
	struct fplinux_terminal_diagnostic *diagnostic,
	const struct fplinux_input_session *input, int control);
bool fplinux_terminal_diagnostic_dispatch(
	struct fplinux_terminal_diagnostic *diagnostic, int control,
	int terminal_vt);
/* Close before resuming input, including returns made by an external keyboard. */
void fplinux_terminal_diagnostic_close(
	struct fplinux_terminal_diagnostic *diagnostic);

#endif
