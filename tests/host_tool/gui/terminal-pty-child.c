/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void write_output(const char *bytes, size_t size)
{
	while (size) {
		ssize_t count = write(STDOUT_FILENO, bytes, size);

		if (count < 0 && errno == EINTR)
			continue;
		assert(count > 0);
		bytes += count;
		size -= (size_t)count;
	}
}

static void keep_job(const char *path)
{
	FILE *file;

	assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
	assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
	file = fopen(path, "w");
	assert(file);
	assert(fprintf(file, "%ld\n", (long)getpid()) > 0);
	assert(!fclose(file));
	for (;;)
		pause();
}

static void read_input(void)
{
	const struct timespec delay = { .tv_nsec = 10000000L };
	struct termios settings;
	size_t expected;
	size_t received = 0;
	FILE *gate;

	assert(!tcgetattr(STDIN_FILENO, &settings));
	cfmakeraw(&settings);
	assert(!tcsetattr(STDIN_FILENO, TCSANOW, &settings));
	write_output("INPUT-READY\r\n", sizeof("INPUT-READY\r\n") - 1);
	while (!(gate = fopen("input-gate", "r"))) {
		assert(errno == ENOENT);
		nanosleep(&delay, NULL);
	}
	assert(fscanf(gate, "%zu", &expected) == 1);
	assert(!fclose(gate));
	while (received < expected) {
		char bytes[4096];
		ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
		size_t index;

		if (count < 0 && errno == EINTR)
			continue;
		assert(count > 0);
		assert((size_t)count <= expected - received);
		for (index = 0; index < (size_t)count; ++index)
			assert(bytes[index] == "q7$Z"[(received + index) % 4]);
		received += (size_t)count;
	}
	write_output("INPUT-EXACT\r\n", sizeof("INPUT-EXACT\r\n") - 1);
}

int main(int argc, char **argv)
{
	const char *scenario;

	if (argc == 3 && !strcmp(argv[1], "job")) {
		keep_job(argv[2]);
		return 0;
	}
	assert(argc == 5);
	assert(!strcmp(argv[1], "--noprofile"));
	assert(!strcmp(argv[2], "--rcfile"));
	assert(!strcmp(argv[4], "-i"));
	scenario = argv[3];
	if (!strcmp(scenario, "backpressure")) {
		read_input();
		return 0;
	}
	if (!strcmp(scenario, "buffered-failure")) {
		char padding[4096];
		unsigned int index;

		memset(padding, '\r', sizeof(padding));
		for (index = 0; index < 48; ++index)
			write_output(padding, sizeof(padding));
	}
	write_output("LAST-LITERAL-OUTPUT\r\n",
		     sizeof("LAST-LITERAL-OUTPUT\r\n") - 1);
	if (!strcmp(scenario, "signal")) {
		raise(SIGTERM);
		return 99;
	}
	if (!strcmp(scenario, "closed-stream")) {
		const struct timespec delay = { .tv_nsec = 10000000L };

		assert(!close(STDIN_FILENO));
		assert(!close(STDOUT_FILENO));
		assert(!close(STDERR_FILENO));
		while (access("exit-gate", F_OK)) {
			assert(errno == ENOENT);
			nanosleep(&delay, NULL);
		}
	}
	return !strcmp(scenario, "success") ? 0 : 37;
}
