// SPDX-License-Identifier: GPL-2.0-only
/* Run MicroPythonOS in a temporary FPLinux framebuffer session. */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fplinux-micropythonos-launcher-internal.h"
#include "fplinux-fb-session.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FPLINUX_MICROPYTHONOS_LAUNCHER_FRAMEBUFFER_DEVICE "/dev/fb0"
#ifndef FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH
#define FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH \
	"/tmp/fplinux-micropythonos.lock"
#endif
#define FPLINUX_MICROPYTHONOS_LAUNCHER_TTY_DEVICE "/dev/tty0"

static volatile sig_atomic_t pending_signal;

static _Noreturn void fplinux_micropythonos_launcher_die(const char *message)
{
	fprintf(stderr, "micropythonos: %s\n", message);
	exit(EXIT_FAILURE);
}

static _Noreturn void
fplinux_micropythonos_launcher_die_errno(const char *message)
{
	fprintf(stderr, "micropythonos: %s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

int fplinux_micropythonos_launcher_acquire_session_lock(void)
{
	const struct flock lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
	};
	int descriptor;

	descriptor = open(FPLINUX_MICROPYTHONOS_LAUNCHER_LOCK_PATH,
			  O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
		fplinux_micropythonos_launcher_die_errno(
			"cannot create session lock");
	if (fcntl(descriptor, F_SETLK, &lock) < 0) {
		int saved_errno = errno;

		if (errno == EACCES || errno == EAGAIN) {
			close(descriptor);
			fplinux_micropythonos_launcher_die(
				"another MicroPythonOS session is already running");
		}
		close(descriptor);
		errno = saved_errno;
		fplinux_micropythonos_launcher_die_errno("cannot lock session");
	}
	return descriptor;
}

static void
fplinux_micropythonos_launcher_save_display(struct fplinux_fb_session *session)
{
	char error[128];

	if (!fplinux_fb_session_open(
		    session, FPLINUX_MICROPYTHONOS_LAUNCHER_FRAMEBUFFER_DEVICE,
		    FPLINUX_MICROPYTHONOS_LAUNCHER_TTY_DEVICE, error,
		    sizeof(error)))
		fplinux_micropythonos_launcher_die(error);
}

static void fplinux_micropythonos_launcher_catch_signal(int signal_number)
{
	pending_signal = signal_number;
}

static void fplinux_micropythonos_launcher_install_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = fplinux_micropythonos_launcher_catch_signal,
	};
	size_t index;

	sigemptyset(&action.sa_mask);
	for (index = 0; index < ARRAY_SIZE(signals); ++index)
		if (sigaction(signals[index], &action, NULL) < 0)
			fplinux_micropythonos_launcher_die_errno(
				"cannot install signal handler");
}

static void fplinux_micropythonos_launcher_reset_signal_handlers(void)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = SIG_DFL,
	};
	size_t index;

	sigemptyset(&action.sa_mask);
	for (index = 0; index < ARRAY_SIZE(signals); ++index)
		if (sigaction(signals[index], &action, NULL) < 0)
			_exit(126);
}

pid_t fplinux_micropythonos_launcher_start_command(char *const command[])
{
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (child == 0) {
		fplinux_micropythonos_launcher_reset_signal_handlers();
		execvp(command[0], command);
		fprintf(stderr, "micropythonos: cannot execute %s: %s\n",
			command[0], strerror(errno));
		_exit(126);
	}
	return child;
}

int fplinux_micropythonos_launcher_wait_for_command(pid_t child)
{
	int status;

	for (;;) {
		if (pending_signal) {
			int signal_number = pending_signal;

			pending_signal = 0;
			if (kill(child, signal_number) < 0 && errno != ESRCH)
				fprintf(stderr,
					"micropythonos: cannot forward signal: %s\n",
					strerror(errno));
		}
		if (waitpid(child, &status, 0) == child)
			break;
		if (errno != EINTR) {
			fprintf(stderr,
				"micropythonos: cannot wait for command: %s\n",
				strerror(errno));
			return EXIT_FAILURE;
		}
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	struct fplinux_fb_session display;
	pid_t child;
	int command_status;
	int lock;
	bool restored;

	if (argc < 2) {
		fprintf(stderr,
			"usage: micropythonos <command> [argument ...]\n");
		return EXIT_FAILURE;
	}
	lock = fplinux_micropythonos_launcher_acquire_session_lock();
	fplinux_micropythonos_launcher_save_display(&display);
	fplinux_micropythonos_launcher_install_signal_handlers();
	{
		char error[128];

		if (!fplinux_fb_session_set_graphics(&display, error,
						     sizeof(error)))
			fplinux_micropythonos_launcher_die(error);
	}
	child = fplinux_micropythonos_launcher_start_command(argv + 1);
	if (child < 0) {
		fprintf(stderr, "micropythonos: cannot start command: %s\n",
			strerror(errno));
		command_status = EXIT_FAILURE;
	} else {
		command_status =
			fplinux_micropythonos_launcher_wait_for_command(child);
	}
	restored = fplinux_fb_session_close(&display);
	close(lock);
	return restored ? command_status : EXIT_FAILURE;
}
