/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Feeds phone keypad taps to the linked console and prints the shell input
 * they queue as hexadecimal. Each argument is CODE@MS: an evdev press and
 * release of key CODE at MS milliseconds on the console's monotonic clock.
 * No VT, PTY or evdev device is opened.
 */

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "fplinux-console-internal.h"

static struct fplinux_console_fifo pty_tx;

static int parse_tap(const char *argument, unsigned *code,
		     struct timespec *time)
{
	unsigned long value;
	unsigned long milliseconds;
	char *end;

	value = strtoul(argument, &end, 0);
	if (end == argument || *end != '@' || value > KEY_MAX)
		return -1;
	argument = end + 1;
	milliseconds = strtoul(argument, &end, 10);
	if (end == argument || *end)
		return -1;
	*code = (unsigned)value;
	time->tv_sec = (time_t)(milliseconds / 1000);
	time->tv_nsec = (long)(milliseconds % 1000) * 1000000L;
	return 0;
}

static void report_key(unsigned code, int value, struct timespec time)
{
	struct input_event events[] = {
		{ .type = EV_KEY, .code = (unsigned short)code, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	};

	fplinux_console_keypad_events(&pty_tx, events,
				      sizeof(events) / sizeof(events[0]), time);
}

int main(int argc, char **argv)
{
	size_t i;
	int argument;

	for (argument = 1; argument < argc; ++argument) {
		struct timespec time;
		unsigned code;

		if (parse_tap(argv[argument], &code, &time) < 0) {
			fprintf(stderr, "invalid tap: %s\n", argv[argument]);
			return 2;
		}
		report_key(code, 1, time);
		report_key(code, 0, time);
	}
	for (i = 0; i < pty_tx.length; ++i)
		printf("%02x",
		       pty_tx.bytes[(pty_tx.head + i) % sizeof(pty_tx.bytes)]);
	putchar('\n');
	return 0;
}
