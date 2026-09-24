/* SPDX-License-Identifier: GPL-2.0-only */
/* Phone keypad input of the local console, also linked by its host harness. */

#ifndef FPLINUX_CONSOLE_INTERNAL_H
#define FPLINUX_CONSOLE_INTERNAL_H

#include <linux/input.h>
#include <stddef.h>
#include <time.h>

#define FPLINUX_CONSOLE_PTY_TX_BYTES 4096

/* Shell input not yet written to the PTY: length bytes from head, wrapping. */
struct fplinux_console_fifo {
	unsigned char bytes[FPLINUX_CONSOLE_PTY_TX_BYTES];
	size_t head;
	size_t length;
};

/*
 * Applies events read together from the phone keypad at monotonic time now
 * and queues the shell input they produce.
 */
void fplinux_console_keypad_events(struct fplinux_console_fifo *pty_tx,
				   const struct input_event *events,
				   size_t count, struct timespec now);

#endif
