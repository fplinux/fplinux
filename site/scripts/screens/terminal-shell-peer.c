/* SPDX-License-Identifier: GPL-2.0-only */
/* Controlled shell output for native Terminal captures. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t resized;

static void window_changed(int signal_number)
{
	(void)signal_number;
	resized = 1;
}

static void write_all(int fd, const char *bytes, size_t size)
{
	while (size) {
		ssize_t count = write(fd, bytes, size);

		if (count < 0 && errno == EINTR)
			continue;
		assert(count > 0);
		bytes += count;
		size -= (size_t)count;
	}
}

static void text(const char *bytes)
{
	write_all(STDOUT_FILENO, bytes, strlen(bytes));
}

static unsigned int report_size(int control)
{
	struct winsize size;

	assert(!ioctl(STDIN_FILENO, TIOCGWINSZ, &size));
	write_all(control, (const char *)&size, sizeof(size));
	return size.ws_col;
}

static void read_text(const char *path, char *bytes, size_t capacity)
{
	FILE *file = fopen(path, "rb");
	size_t size;

	assert(file);
	size = fread(bytes, 1, capacity - 1, file);
	assert(!ferror(file) && feof(file));
	assert(!fclose(file));
	bytes[size] = '\0';
}

static void render_sample(char request)
{
	struct winsize size;
	char position[64];

	assert(!ioctl(STDIN_FILENO, TIOCGWINSZ, &size));
	assert(size.ws_col >= 6 && size.ws_row >= 2);
	text("\033[0m\033[?7h\033[?25h\033[2J\033[H");
	if (request == 'P') {
		text("gpqy_Z\033[2;1H\033[?25l");
	} else if (request == 'I') {
		text("\033[7m \033[0m\033[H");
	} else if (request == 'B') {
		text("\033[107m \033[0m\033[H");
	} else {
		assert(request == 'E' || request == 'L');
		if (request == 'L') {
			text("gpqy_");
			assert(snprintf(position, sizeof(position),
					"\033[%u;1H",
					size.ws_row) < (int)sizeof(position));
			text(position);
		}
		for (unsigned int column = 1; column < size.ws_col; ++column)
			text("W");
		text("Z");
	}
}

int main(void)
{
	const char *control_text = getenv("FPLINUX_SCREEN_CONTROL_FD");
	struct termios settings;
	struct sigaction action = { .sa_handler = window_changed };
	char issue[512];
	char hostname[64];
	char prompt[128];
	unsigned int columns;
	int control;

	assert(control_text);
	control = atoi(control_text);
	assert(control > STDERR_FILENO);
	assert(!tcgetattr(STDIN_FILENO, &settings));
	cfmakeraw(&settings);
	assert(!tcsetattr(STDIN_FILENO, TCSANOW, &settings));
	assert(!sigemptyset(&action.sa_mask));
	assert(!sigaction(SIGWINCH, &action, NULL));
	columns = report_size(control);
	read_text(getenv("FPLINUX_SCREEN_ISSUE"), issue, sizeof(issue));
	read_text(getenv("FPLINUX_SCREEN_HOSTNAME"), hostname,
		  sizeof(hostname));
	hostname[strcspn(hostname, "\r\n")] = '\0';
	if (columns < 30)
		snprintf(prompt, sizeof(prompt), "~$ ");
	else
		snprintf(prompt, sizeof(prompt), "fplinux@%s:~$ ", hostname);
	text(prompt);
	text("cat /etc/issue\r\n");
	for (const char *next = issue; *next; ++next) {
		if (*next == '\n')
			text("\r");
		write_all(STDOUT_FILENO, next, 1);
	}
	text(prompt);
	text("uname -s\r\nLinux\r\n");
	text(prompt);
	write_all(control, "R", 1);
	for (;;) {
		struct pollfd inputs[] = {
			{ .fd = STDIN_FILENO, .events = POLLIN },
			{ .fd = control, .events = POLLIN },
		};
		char bytes[128];
		ssize_t count;
		int ready;

		if (resized) {
			resized = 0;
			report_size(control);
		}
		ready = poll(inputs, 2, 100);
		if (ready < 0 && errno == EINTR)
			continue;
		assert(ready >= 0);
		if (!ready)
			continue;
		if (inputs[1].revents & POLLIN) {
			assert(read(control, bytes, 1) == 1);
			if (strchr("PIBEL", bytes[0])) {
				render_sample(bytes[0]);
				write_all(control, bytes, 1);
				continue;
			}
			if (bytes[0] == 'F' || bytes[0] == 'S') {
				text("\r\nFINAL-SHELL-OUTPUT");
				write_all(control, "Q", 1);
				if (bytes[0] == 'S')
					raise(SIGTERM);
				return 37;
			}
			assert(bytes[0] == 'H');
			for (unsigned int line = 0; line < 600; ++line)
				text("history line\r\n");
			write_all(control, "H", 1);
		}
		if (!(inputs[0].revents & (POLLIN | POLLHUP)))
			continue;
		count = read(STDIN_FILENO, bytes, sizeof(bytes));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		write_all(STDOUT_FILENO, bytes, (size_t)count);
		write_all(control, bytes, (size_t)count);
	}
	return 0;
}
