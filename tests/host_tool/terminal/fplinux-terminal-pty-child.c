/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void write_output(const char *bytes, size_t size)
{
	size_t used = 0;

	while (used < size) {
		ssize_t written =
			write(STDOUT_FILENO, bytes + used, size - used);

		if (written < 0 && errno == EINTR)
			continue;
		assert(written > 0);
		used += (size_t)written;
	}
}

int main(int argc, char **argv)
{
	const struct timespec delay = { .tv_nsec = 10000000L };
	int marker;
	int status;

	/* The production PTY launches its shell with a startup-file argument. */
	assert(argc == 5);
	assert(!strcmp(argv[2], "--rcfile"));
	assert(!strcmp(argv[3], "pty-success") ||
	       !strcmp(argv[3], "pty-failure") ||
	       !strcmp(argv[3], "pty-buffered-failure"));
	status = !strcmp(argv[3], "pty-success") ? 0 : 37;
	if (!strcmp(argv[3], "pty-buffered-failure")) {
		char padding[4096];

		memset(padding, '\r', sizeof(padding));
		for (unsigned int chunk = 0; chunk < 32; ++chunk)
			write_output(padding, sizeof(padding));
	}
	write_output("\033[?25la", 7);
	assert(!close(STDIN_FILENO));
	assert(!close(STDOUT_FILENO));
	assert(!close(STDERR_FILENO));
	marker = open("terminal-stream-closed",
		      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(marker >= 0);
	assert(!close(marker));
	/* A display event must remain dispatchable after the stream closes. */
	for (unsigned int attempt = 0; attempt < 200; ++attempt) {
		if (!access("terminal-vt-dispatched", F_OK))
			return status;
		assert(errno == ENOENT);
		assert(!nanosleep(&delay, NULL));
	}
	assert(!"display dispatch did not release the child");
	return status;
}
