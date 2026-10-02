/* SPDX-License-Identifier: GPL-2.0-only */
/* Pipes substitute for evdev streams; VT ioctls use an in-memory active VT. */
#define _GNU_SOURCE
#include "terminal-diagnostic.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int active_vt;
static int hint_fd;

int __wrap_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "/dev/tty1"));
	assert((flags & O_ACCMODE) == O_WRONLY);
	return fcntl(hint_fd, F_DUPFD_CLOEXEC, 0);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;

	(void)fd;
	va_start(arguments, request);
	if (request == VT_GETSTATE) {
		struct vt_stat *state = va_arg(arguments, struct vt_stat *);

		memset(state, 0, sizeof(*state));
		state->v_active = active_vt;
	} else {
		assert(request == VT_ACTIVATE);
		active_vt = va_arg(arguments, int);
	}
	va_end(arguments);
	return 0;
}

static void init_input(struct fplinux_input_session *input)
{
	memset(input, 0, sizeof(*input));
	for (size_t index = 0; index < FPLINUX_INPUT_SESSION_DEVICE_COUNT;
	     ++index)
		input->devices[index].fd = -1;
}

static void key(int fd, unsigned short code, int value)
{
	struct input_event event = {
		.type = EV_KEY,
		.code = code,
		.value = value,
	};

	assert(write(fd, &event, sizeof(event)) == sizeof(event));
}

static void dispatch(struct fplinux_terminal_diagnostic *diagnostic,
		     int terminal_vt)
{
	assert(fplinux_terminal_diagnostic_dispatch(diagnostic, -1,
						    terminal_vt));
}

static void return_to_shell(void)
{
	struct fplinux_terminal_diagnostic diagnostic = { 0 };
	struct fplinux_input_session input;
	char hint[128] = { 0 };
	int keypad[2];
	int duplicate;

	init_input(&input);
	assert(pipe2(keypad, O_NONBLOCK | O_CLOEXEC) == 0);
	input.devices[0].fd = keypad[0];
	input.devices[0].source = FPLINUX_INPUT_SOURCE_KEYPAD;
	active_vt = 7;
	assert(fplinux_terminal_diagnostic_begin(&diagnostic, &input, -1));
	assert(active_vt == 1);
	assert(pread(hint_fd, hint, sizeof(hint) - 1, 0) > 0);
	assert(strstr(hint, "Right soft: return to terminal"));
	duplicate = diagnostic.fds[0];
	assert(fcntl(duplicate, F_GETFD) & FD_CLOEXEC);
	/* Model the session closing its original descriptor during suspend. */
	close(keypad[0]);
	key(keypad[1], KEY_F14, 0);
	dispatch(&diagnostic, 7);
	assert(active_vt == 1);
	key(keypad[1], KEY_F14, 1);
	key(keypad[1], KEY_F14, 2);
	dispatch(&diagnostic, 7);
	assert(active_vt == 1);
	key(keypad[1], KEY_F14, 0);
	dispatch(&diagnostic, 7);
	assert(active_vt == 7);
	assert(diagnostic.count == 0);
	assert(fcntl(duplicate, F_GETFD) == -1 && errno == EBADF);
	fplinux_terminal_diagnostic_close(&diagnostic);
	close(keypad[1]);
}

static void keypad_sources_only(void)
{
	struct fplinux_terminal_diagnostic diagnostic = { 0 };
	struct fplinux_input_session input;
	struct input_event external_event;
	int streams[3][2];

	init_input(&input);
	for (size_t index = 0; index < 3; ++index) {
		assert(pipe2(streams[index], O_NONBLOCK | O_CLOEXEC) == 0);
		input.devices[index].fd = streams[index][0];
		input.devices[index].source =
			index == 1 ? FPLINUX_INPUT_SOURCE_KEYBOARD :
				     FPLINUX_INPUT_SOURCE_KEYPAD;
	}
	active_vt = 11;
	assert(fplinux_terminal_diagnostic_begin(&diagnostic, &input, -1));
	key(streams[1][1], KEY_F14, 1);
	key(streams[1][1], KEY_F14, 0);
	key(streams[0][1], KEY_F13, 1);
	key(streams[0][1], KEY_F13, 0);
	dispatch(&diagnostic, 11);
	assert(active_vt == 1);
	assert(read(streams[1][0], &external_event, sizeof(external_event)) ==
	       sizeof(external_event));
	assert(external_event.code == KEY_F14 && external_event.value == 1);
	assert(read(streams[1][0], &external_event, sizeof(external_event)) ==
	       sizeof(external_event));
	assert(external_event.code == KEY_F14 && external_event.value == 0);
	/* A release on a different keypad must not complete the first one's tap. */
	key(streams[0][1], KEY_F14, 1);
	key(streams[2][1], KEY_F14, 0);
	dispatch(&diagnostic, 11);
	assert(active_vt == 1);
	key(streams[2][1], KEY_F14, 1);
	key(streams[2][1], KEY_F14, 0);
	dispatch(&diagnostic, 11);
	assert(active_vt == 11);
	for (size_t index = 0; index < 3; ++index) {
		assert(fcntl(streams[index][0], F_GETFD) >= 0);
		close(streams[index][0]);
		close(streams[index][1]);
	}
}

static void other_vt_and_close(void)
{
	struct fplinux_terminal_diagnostic diagnostic = { 0 };
	struct fplinux_input_session input;
	struct input_event event;
	int keypad[2];
	int replacement[2];
	int watched;

	init_input(&input);
	assert(pipe2(keypad, O_NONBLOCK | O_CLOEXEC) == 0);
	input.devices[0].fd = keypad[0];
	input.devices[0].source = FPLINUX_INPUT_SOURCE_KEYPAD;
	assert(fplinux_terminal_diagnostic_begin(&diagnostic, &input, -1));
	active_vt = 3;
	key(keypad[1], KEY_F14, 1);
	key(keypad[1], KEY_F14, 0);
	dispatch(&diagnostic, 9);
	assert(active_vt == 3);

	/* An external VT return closes the watch before the session resumes. */
	watched = diagnostic.fds[0];
	fplinux_terminal_diagnostic_close(&diagnostic);
	assert(fcntl(watched, F_GETFD) == -1 && errno == EBADF);
	assert(fcntl(keypad[0], F_GETFD) >= 0);
	assert(pipe2(replacement, O_NONBLOCK | O_CLOEXEC) == 0);
	assert(dup2(replacement[0], watched) == watched);
	key(replacement[1], KEY_F14, 1);
	key(replacement[1], KEY_F14, 0);
	active_vt = 1;
	dispatch(&diagnostic, 9);
	assert(active_vt == 1);
	assert(read(watched, &event, sizeof(event)) == sizeof(event));
	assert(event.code == KEY_F14 && event.value == 1);
	close(watched);
	if (replacement[0] != watched)
		close(replacement[0]);
	close(replacement[1]);
	close(keypad[0]);
	close(keypad[1]);
}

int main(int argc, char **argv)
{
	FILE *hint = tmpfile();

	assert(argc == 2 && hint);
	hint_fd = fileno(hint);
	if (!strcmp(argv[1], "return"))
		return_to_shell();
	else if (!strcmp(argv[1], "sources"))
		keypad_sources_only();
	else if (!strcmp(argv[1], "ownership"))
		other_vt_and_close();
	else
		abort();
	fclose(hint);
	return EXIT_SUCCESS;
}
